#include "editor/editor.h"

#include "asset/cell_format.h"
#include "asset/mesh_format.h"
#include "asset/terrain_format.h"
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
#include "tools/frame_debugger.h"
#include "tools/stats_panel.h"
#include "world/cell.h"
#include "world/prefab.h"
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
#include <numbers>
#include <span>
#include <string_view>
#include <system_error>

#if DE_BUILD_DEV
namespace de::editor {

namespace {

constexpr const char* kLayoutPath = "user:/editor/layout.ini";
constexpr const char* kSettingsPath = "user:/editor/editor.settings";
constexpr const char* kBookmarksPath = "user:/editor/bookmarks.settings";
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
    bool cell_grid = false;
    bool snap_ground = false;
    bool colliders = false;
    bool selected_colliders = false;
    bool cook_auto = true;
    uint32_t fps_cap = 0;
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
        } else if (key == "cell_grid") {
            settings.cell_grid = on;
        } else if (key == "snap_ground") {
            settings.snap_ground = on;
        } else if (key == "colliders") {
            settings.colliders = on;
        } else if (key == "selected_colliders") {
            settings.selected_colliders = on;
        } else if (key == "cook_auto") {
            settings.cook_auto = on;
        } else if (key == "fps_cap") {
            settings.fps_cap = static_cast<uint32_t>(std::clamp(to_number(value, 0.0f), 0.0f, 1000.0f));
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
    char text[640];
    const int length = std::snprintf(
        text, sizeof(text),
        "# The editor's interface scale (on top of the display's), its tool, its axes, where its handles "
        "sit and its snapping.\n"
        "ui_scale %.2f\ntool %s\nspace %s\nhandles %s\nsnap_move %s %g\nsnap_rotate %s %g\n"
        "snap_scale %s %g\ndrawer %.2f\ncell_grid %s\nsnap_ground %s\ncolliders %s\nselected_colliders %s\n"
        "cook_auto %s\nfps_cap %u\n",
        static_cast<double>(settings.ui_scale), kTools[static_cast<int>(settings.tool)],
        settings.space == Space::Local ? "local" : "world",
        settings.handles_at == HandlesAt::Center ? "center" : "pivot", settings.snap.move ? "on" : "off",
        static_cast<double>(settings.snap.move_m), settings.snap.rotate ? "on" : "off",
        static_cast<double>(settings.snap.rotate_deg), settings.snap.scale ? "on" : "off",
        static_cast<double>(settings.snap.scale_step), static_cast<double>(settings.drawer_share),
        settings.cell_grid ? "on" : "off", settings.snap_ground ? "on" : "off", settings.colliders ? "on" : "off",
        settings.selected_colliders ? "on" : "off", settings.cook_auto ? "on" : "off", settings.fps_cap);
    if (length > 0) {
        static_cast<void>(vfs.write_file(kSettingsPath, std::as_bytes(std::span(text, static_cast<size_t>(length)))));
    }
}

// The world each project had open last, for the next start with no world named: a line per project, the world's ID
// and then the project's folder.
constexpr const char* kLastWorldsPath = "user:/editor/last_worlds.settings";

asset::AssetId last_world(const platform::Vfs& vfs) {
    const char* project = vfs.mount_root(platform::Mount::Src);
    if (project == nullptr || !vfs.exists(kLastWorldsPath)) {
        return {};
    }
    const auto file = vfs.read_whole_file(kLastWorldsPath, editor_heap());
    if (!file) {
        return {};
    }
    std::string_view text = core::skip_bom(std::string_view(reinterpret_cast<const char*>(file->data()), file->size()));
    while (!text.empty()) {
        const std::string_view line = core::strip_comment(core::next_line(text));
        const size_t space = line.find(' ');
        asset::AssetId id;
        if (space != std::string_view::npos && core::trim(line.substr(space)) == project &&
            asset::parse_asset_id(line.substr(0, space), id)) {
            return id;
        }
    }
    return {};
}

void remember_world(const platform::Vfs& vfs, asset::AssetId world) {
    const char* project = vfs.mount_root(platform::Mount::Src);
    if (project == nullptr || !world.is_valid()) {
        return;
    }
    // The other projects' lines as they are, then this one's.
    core::Array<char> kept(editor_heap());
    const auto add = [&kept](std::string_view text) {
        for (const char c : text) {
            kept.push(c);
        }
    };
    add("# The world each project had open last in the editor: its ID, then the project's folder.\n");
    if (const auto file = vfs.read_whole_file(kLastWorldsPath, editor_heap())) {
        std::string_view text =
            core::skip_bom(std::string_view(reinterpret_cast<const char*>(file->data()), file->size()));
        while (!text.empty()) {
            const std::string_view line = core::strip_comment(core::next_line(text));
            const size_t space = line.find(' ');
            if (space != std::string_view::npos && core::trim(line.substr(space)) != project) {
                add(line);
                add("\n");
            }
        }
    }
    add(asset::format_asset_id(world).view());
    add(" ");
    add(project);
    add("\n");
    static_cast<void>(vfs.write_file(kLastWorldsPath, std::as_bytes(kept.span())));
}

// The frame rate limits Preferences offers; 0 is none.
constexpr uint32_t kFrameRateLimits[] = {0, 30, 60, 120, 240};

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

// A text button of the status bar: the accent while on, a step lighter under the mouse.
float status_button_width(const char* label) {
    return (2.0f * px(8.0f)) + ImGui::CalcTextSize(label).x;
}

bool status_button(const char* id, const char* label, bool on) {
    const float height = ImGui::GetFrameHeight();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 size(status_button_width(label), height);
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const ThemeColor background = on        ? (hovered ? ThemeColor::AccentHovered : ThemeColor::Accent)
                                  : held    ? ThemeColor::ControlActive
                                  : hovered ? ThemeColor::ControlHovered
                                            : ThemeColor::Control;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, ImVec2(min.x + size.x, min.y + size.y), color_u32(background),
                        px(theme().metrics.control_rounding));
    draw->AddText(ImVec2(min.x + px(8.0f), std::floor(min.y + ((height - ImGui::GetFontSize()) * 0.5f))),
                  color_u32(on || hovered ? ThemeColor::TextEmphasis : ThemeColor::Text), label);
    return pressed;
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
      selected_entities_(editor_heap()), properties_(editor_heap()), thumbnails_(editor_heap()),
      pictures_(editor_heap()), effect_viewer_(editor_heap()), prefab_editor_(editor_heap()), browser_(editor_heap()),
      cell_sources_(editor_heap()), cooking_(editor_heap()), holds_(editor_heap()), members_(editor_heap()),
      scratch_entities_(editor_heap()) {}

bool Editor::startup(app::Engine& engine, const app::FrameworkArgs& args) {
    // What this project had open last, unless --world names one. Not for a picture's run or a counted one.
    if (!engine.scripted && args.frames == 0) {
        viewer_.set_first_world(last_world(engine.vfs));
    }
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
    engine.console.add_command("editor.cook", "",
                               "Cook the whole project in the background (as the status bar's Cook and Ctrl+B do).",
                               &Editor::cook_command, this);
    engine.console.add_variable("editor.cook_auto",
                                "Cook by itself when the project's files change (the status bar's Cook automatically).",
                                &cook_auto_, this);
    engine.console.add_command("editor.open_world", "[name]",
                               "Open a world of the project by its name under content/ (as File > Open World does); "
                               "with no name, lists them.",
                               &Editor::open_world_command, this);
    engine.console.add_command("editor.open_project", "[name]",
                               "Open a project beside the engine in an editor of its own, closing this one (as File > "
                               "Open Project does); with no name, lists them.",
                               &Editor::open_project_command, this);
    engine.console.add_command("editor.move", "<name> <x> <y> <z>",
                               "Move the entity of that name to a place in meters (undoable), and select it.",
                               &Editor::move_command, this);
    engine.console.add_command("editor.place", "<src:/content/...model, .vfx or .prefab> <x> <y> <z>",
                               "Place a model, an effect or a prefab there (meters), as dropping it on the Playground "
                               "does.",
                               &Editor::place_command, this);
    engine.console.add_command("editor.view_effect", "<src:/content/...vfx>",
                               "Show an effect in the effect viewer (as a double click on it in Resources does).",
                               &Editor::view_effect_command, this);
    engine.console.add_command(
        "editor.effect",
        "[play | hold | step N | seek S | speed X | loop on|off | length S | hide MASK | frame | close]",
        "What the effect viewer's bar does: play or hold it, step N of the simulation's steps (back for less than 0), "
        "set its age in seconds, its speed, its loop and the bar's length, leave emitters out (a bit each), frame it, "
        "close it. Alone: where it is.",
        &Editor::effect_command, this);
    engine.console.add_command("editor.edit_prefab", "<src:/content/...prefab>",
                               "Open a prefab in the prefab editor (as a double click on it in Resources does).",
                               &Editor::edit_prefab_command, this);
    engine.console.add_command("editor.new_prefab", "<src:/content/...prefab>",
                               "Open a new prefab in the prefab editor (as Resources' New Prefab does): it's a file "
                               "once it's saved.",
                               &Editor::new_prefab_command, this);
    engine.console.add_command("editor.add_light", "[x y z]",
                               "Add a point light (as Edit > Add Light does): at a place in meters, or a little above "
                               "where the middle of the Playground meets the ground.",
                               &Editor::add_light_command, this);
    engine.console.add_command("editor.prefab_from_selection", "<src:/content/...prefab>",
                               "Open a new prefab in the prefab editor whose parts are the chosen things (as Edit > "
                               "Make Prefab from Selection does): it's a file once it's saved, and they stay as they "
                               "are.",
                               &Editor::prefab_from_selection_command, this);
    engine.console.add_command(
        "editor.prefab",
        "[add PATH [X Y Z] | remove ID | duplicate ID | rename ID NEW | move ID X Y Z | rotate ID X Y Z | "
        "scale ID X Y Z | light ID off | light ID INTENSITY RADIUS_M [R G B] | select [ID] | variant NAME | "
        "delete_variant NAME | keep NAME PART on|off | show [NAME] | undo | redo | save | frame | close]",
        "What the prefab editor's window does to the prefab open in it: add a part from a model, an effect or another "
        "prefab, take one out, copy or rename it, place it (meters, degrees, times), choose one; add or delete a "
        "variant, say whether it keeps a part, show one; undo, redo, save, frame, close. Alone: what it holds.",
        &Editor::prefab_command, this);
    engine.console.add_command(
        "editor.drop", "<src:/content/...model or .vfx>",
        "Drop a model or an effect at the middle of the Playground, on the ground there (as the mouse would).",
        &Editor::drop_command, this);
    engine.console.add_command("editor.duplicate", "[name ...]", "Duplicate the entity of that name (as Ctrl+D does).",
                               &Editor::duplicate_command, this);
    engine.console.add_command("editor.delete", "[name ...]",
                               "Delete the entity of that name (as the Delete key does).", &Editor::delete_command,
                               this);
    engine.console.add_command("editor.ground", "[name ...]",
                               "Drop the entity of that name onto the ground below it (as End does).",
                               &Editor::ground_command, this);
    engine.console.add_command("editor.move_file", "<from> <to>",
                               "Move or rename a file or folder in content/ (src:/content/... paths), with its meta, "
                               "rewriting what names it.",
                               &Editor::move_file_command, this);
    engine.console.add_command("editor.new_folder", "<path>", "Make a folder in content/ (a src:/content/... path).",
                               &Editor::new_folder_command, this);
    engine.console.add_command("editor.delete_file", "<path ...>",
                               "Send files or folders in content/ to the recycle bin with their metas, saying what "
                               "still names them.",
                               &Editor::delete_file_command, this);
    engine.console.add_command("editor.frame", "",
                               "Frame the selection, as F: the camera eases back until it's in view.",
                               &Editor::frame_command, this);
    engine.console.add_command(
        "editor.select", "[name ...]",
        "Choose the entities of those names (the last the primary), or Terrain; none: nothing chosen.",
        &Editor::select_command, this);
    engine.console.add_command("editor.folder", "<folder> [name ...]",
                               "Put the entities of those names (or the selection) in a folder, \"\" for none.",
                               &Editor::folder_command, this);
    engine.console.add_command("editor.bookmark", "<1-9> [set]",
                               "Go to a camera bookmark (as 1 to 9), or keep where the camera is in it (as Ctrl with "
                               "them).",
                               &Editor::bookmark_command, this);
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
    show_cells_ = settings.cell_grid;
    snap_ground_ = settings.snap_ground;
    engine.show_colliders = settings.colliders;
    engine.show_selected_colliders = settings.selected_colliders;
    cook_auto_ = settings.cook_auto;
    fps_cap_kept_ = settings.fps_cap;
    if (app::flag_value(args, "--fps-cap") == nullptr) {
        engine.fps_cap = fps_cap_kept_; // Preferences': a flag says otherwise for one run, and isn't kept
    }
    // Nothing cooks by itself in a picture's run or a counted one.
    watching_ = !engine.scripted && args.frames == 0;
    if (watching_) {
        remember_world(engine.vfs, viewer_.world_id());
    }
    show_cells_ = show_cells_ || app::has_flag(args, "--cell-grid");
    if (const char* typed = app::flag_value(args, "--cmd"); typed != nullptr && engine.console_ui != nullptr) {
        engine.console_ui->set_command_line(typed);
    }
    if (const char* drawer = app::flag_value(args, "--drawer")) {
        drawer_ = std::string_view(drawer) == "output" ? Drawer::Output : Drawer::Resources;
    }
    select_name_ = app::flag_value(args, "--select");
    preview_flag_ = app::flag_value(args, "--preview");
    show_panel_ = app::flag_value(args, "--show");
    if (const char* file = app::flag_value(args, "--select-file")) {
        selection_.select_file(file);
        browser_.show_file(file);
    }
    if (const char* effect = app::flag_value(args, "--view-effect")) {
        if (!view_effect(engine, effect)) {
            DE_LOG_ERROR("editor", "--view-effect: %s", message_);
        }
    }
    if (const char* prefab = app::flag_value(args, "--edit-prefab")) {
        if (!edit_prefab(engine, prefab)) {
            DE_LOG_ERROR("editor", "--edit-prefab: %s", message_);
        }
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
    browser_.set_thumbnails(&thumbnails_);
    properties_.set_thumbnails(&thumbnails_);
    load_layout(engine.vfs);
    load_bookmarks(engine.vfs);
    DE_LOG_INFO("editor", "editing world %s", asset::format_asset_id(viewer_.world_id()).c_str());
    return true;
}

void Editor::save_user_settings(app::Engine& engine) {
    if (!keep_settings_) {
        return;
    }
    save_layout(engine.vfs);
    browser_.save_settings(engine.vfs);
    save_settings(engine.vfs, {.ui_scale = ui_scale_,
                               .tool = tool_,
                               .space = space_,
                               .handles_at = handles_at_,
                               .snap = snap_,
                               .drawer_share = drawer_share_,
                               .cell_grid = show_cells_,
                               .snap_ground = snap_ground_,
                               .colliders = engine.show_colliders,
                               .selected_colliders = engine.show_selected_colliders,
                               .cook_auto = cook_auto_,
                               .fps_cap = fps_cap_kept_});
}

void Editor::shutdown(app::Engine& engine) {
    watch_.stop(engine.jobs);
    save_user_settings(engine);
    commands_.clear();
    release_holds();
    effect_viewer_.shutdown(picture_engine(engine));
    prefab_editor_.shutdown(picture_engine(engine));
    pictures_.shutdown(picture_engine(engine));
    viewer_.shutdown(engine);
}

void Editor::draw(app::Engine& /*engine*/) {
    pictures_.draw();
    effect_viewer_.draw();
    prefab_editor_.draw();
}

void Editor::show_in_resources(std::string_view path) {
    browser_.show_file(path);
    drawer_ = Drawer::Resources;
}

void Editor::update_sources(app::Engine& engine) {
    const flecs::entity_t chosen = selection_.kind == Selection::Kind::Entity ? selection_.entity : 0;
    if (chosen == sources_of_) {
        return;
    }
    sources_of_ = chosen;
    EntitySources sources;
    sources.entity = chosen;
    const flecs::world& ecs = engine.world.ecs();
    if (chosen != 0 && ecs.is_alive(chosen)) {
        core::FixedString<31> variant;
        bool solid = false;
        static_cast<void>(read_sources(ecs.entity(chosen), sources, variant, solid));
    }
    properties_.set_sources(sources);
}

bool Editor::read_sources(flecs::entity entity, EntitySources& sources, core::FixedString<31>& variant, bool& solid) {
    // Its entry's own words for them (in its cell's file, or as the editor made it), from that file's folder.
    EntityRecord recorded;
    record(entity, recorded);
    const std::string_view object = recorded.entry.object;
    constexpr std::string_view kOpen = R"({"entities": [)";
    constexpr std::string_view kClose = "]}";
    core::Array<char> wrapped(editor_heap());
    wrapped.resize(kOpen.size() + object.size() + kClose.size());
    std::memcpy(wrapped.data(), kOpen.data(), kOpen.size());
    std::memcpy(wrapped.data() + kOpen.size(), object.data(), object.size());
    std::memcpy(wrapped.data() + kOpen.size() + object.size(), kClose.data(), kClose.size());
    CellSource alone(editor_heap());
    if (!recorded.has_entry || !alone.parse({wrapped.data(), wrapped.size()}) || alone.entity_count() != 1) {
        return false;
    }
    const auto named = [&](std::string_view key, core::FixedString<255>& out) {
        const std::string_view relative = alone.string_member(0, key);
        char full[platform::kMaxPathLength];
        size_t length = 0;
        if (!relative.empty() && joined_path(recorded.entry.folder, relative, full, length)) {
            static_cast<void>(out.assign(std::string_view(full, length)));
        }
    };
    named("prefab", sources.prefab);
    named("mesh", sources.mesh);
    named("effect", sources.effect);
    static_cast<void>(variant.assign(alone.string_member(0, "variant")));
    solid = alone.string_member(0, "collision") == "mesh";
    return true;
}

bool Editor::prefab_from_selection(app::Engine& engine, std::string_view path) {
    const flecs::world& ecs = engine.world.ecs();
    core::Array<flecs::entity_t> roots(editor_heap());
    chosen_roots(engine, roots);
    if (roots.empty()) {
        static_cast<void>(std::snprintf(message_, sizeof(message_),
                                        "Choose what the prefab is made of first: nothing placed is chosen"));
        return false;
    }
    // Its origin: where the one the handles are on stands, in the world's axes.
    flecs::entity_t first = roots[0];
    for (const flecs::entity_t id : roots) {
        first = id == selection_.entity ? id : first;
    }
    const auto* origin_at = ecs.entity(first).try_get<world::WorldTransform>();
    const DVec3 origin = origin_at != nullptr ? origin_at->position : DVec3(0.0);
    const size_t slash = path.rfind('/');
    const std::string_view folder = slash == std::string_view::npos ? std::string_view() : path.substr(0, slash);
    core::Array<PrefabDocPart> parts(editor_heap());
    size_t left_out = 0;
    for (const flecs::entity_t id : roots) {
        const flecs::entity entity = ecs.entity(id);
        const auto* at = entity.try_get<world::WorldTransform>();
        EntitySources sources;
        core::FixedString<31> variant;
        bool solid = false;
        PrefabDocPart part;
        char relative[256];
        size_t length = 0;
        const auto take = [&](std::string_view full, core::FixedString<255>& out) {
            return !full.empty() && relative_path(folder, full, relative, length) &&
                   out.assign(std::string_view(relative, length));
        };
        bool any = false;
        if (at != nullptr && read_sources(entity, sources, variant, solid)) {
            if (take(sources.prefab.view(), part.prefab)) {
                part.variant = variant;
                any = true;
            } else {
                any = take(sources.mesh.view(), part.mesh);
                any = take(sources.effect.view(), part.effect) || any;
                part.collision = solid && !part.mesh.empty();
            }
        }
        if (!any) {
            ++left_out; // nothing of it to make a part from (a fog volume, a light, a thing with no entry)
            continue;
        }
        char label[128];
        entity_label(engine.assets, entity, label);
        static_cast<void>(part.id.assign(std::string_view(label).substr(0, part.id.capacity())));
        part.position = Vec3(at->position - origin);
        part.set_rotation(at->rotation);
        part.scale = at->scale;
        parts.push(part);
    }
    if (parts.empty()) {
        static_cast<void>(std::snprintf(message_, sizeof(message_),
                                        "Nothing chosen has a model, an effect or a prefab to make a part of"));
        return false;
    }
    if (!prefab_editor_.open_new_with(picture_engine(engine), path, parts.span(), message_)) {
        return false;
    }
    static_cast<void>(std::snprintf(message_, sizeof(message_),
                                    "A new prefab of %zu parts%s: save it in the prefab editor, then place it from "
                                    "Resources",
                                    parts.size(), left_out > 0 ? " (some chosen had nothing to make a part of)" : ""));
    return true;
}

void Editor::prefab_from_selection_command(void* user, const console::Args& args, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    if (args.size() != 1) {
        out.error("editor.prefab_from_selection takes the src:/ path the prefab will have");
        return;
    }
    if (!editor->prefab_from_selection(*editor->engine_, args[0])) {
        out.error("%s", editor->message_);
        return;
    }
    out.info("%s", editor->message_);
}

void Editor::ask_make_prefab(app::Engine& engine) {
    // Beside what its first part is made from: the folder its name goes in.
    const flecs::world& ecs = engine.world.ecs();
    core::Array<flecs::entity_t> roots(editor_heap());
    chosen_roots(engine, roots);
    make_prefab_folder_.clear();
    for (const flecs::entity_t id : roots) {
        EntitySources sources;
        core::FixedString<31> variant;
        bool solid = false;
        if (!make_prefab_folder_.empty() || !read_sources(ecs.entity(id), sources, variant, solid)) {
            continue;
        }
        const std::string_view made_from = !sources.prefab.empty() ? sources.prefab.view()
                                           : !sources.mesh.empty() ? sources.mesh.view()
                                                                   : sources.effect.view();
        if (const size_t slash = made_from.rfind('/'); slash != std::string_view::npos) {
            static_cast<void>(make_prefab_folder_.assign(made_from.substr(0, slash)));
        }
    }
    if (make_prefab_folder_.empty()) {
        static_cast<void>(std::snprintf(message_, sizeof(message_),
                                        "Nothing chosen has a model, an effect or a prefab to make a part of"));
        return;
    }
    static_cast<void>(std::snprintf(make_prefab_text_, sizeof(make_prefab_text_), "new_prefab"));
    open_make_prefab_ = true;
}

void Editor::make_prefab_prompt(app::Engine& engine) {
    constexpr const char* kPrompt = "Make Prefab from Selection";
    if (open_make_prefab_) {
        ImGui::OpenPopup(kPrompt);
        open_make_prefab_ = false;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(16.0f), px(14.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8.0f), px(10.0f)));
    if (ImGui::BeginPopupModal(kPrompt, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        const std::string_view folder = make_prefab_folder_.view().substr(std::string_view("src:/").size());
        ImGui::Text("A new prefab of what's chosen, in %.*s", static_cast<int>(folder.size()), folder.data());
        ImGui::TextColored(color(ThemeColor::TextSecondary),
                           "It opens in the prefab editor. What's chosen stays as it is.");
        ImGui::SetNextItemWidth(px(320.0f));
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        const bool entered = ImGui::InputText("##name", make_prefab_text_, sizeof(make_prefab_text_),
                                              ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        // Its name with the extension, whether that was typed or not.
        char cleaned[128];
        size_t length = 0;
        core::FixedString<255> made = make_prefab_folder_;
        bool valid = clean_file_name(make_prefab_text_, cleaned, length);
        std::string_view name(cleaned, valid ? length : 0);
        if (name.ends_with(".prefab")) {
            name.remove_suffix(std::string_view(".prefab").size());
        }
        valid = valid && !name.empty() && made.append('/') && made.append(name) && made.append(".prefab");
        const bool taken = valid && engine.vfs.exists(made.c_str());
        if (!valid) {
            ImGui::TextColored(color(ThemeColor::TextSecondary), "Lowercase letters, digits, _ - and . only");
        } else if (taken) {
            ImGui::TextColored(color(ThemeColor::Warning), "%.*s.prefab is there already",
                               static_cast<int>(name.size()), name.data());
        } else {
            ImGui::TextColored(color(ThemeColor::TextSecondary), "%.*s.prefab: it's a file once it's saved",
                               static_cast<int>(name.size()), name.data());
        }
        const ImVec2 button(px(96.0f), 0.0f);
        ImGui::BeginDisabled(!valid || taken);
        const bool make = (ImGui::Button("Make", button) || entered) && valid && !taken;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", button) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        } else if (make) {
            static_cast<void>(prefab_from_selection(engine, made.view())); // message_ says what came of it
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

bool Editor::view_effect(app::Engine& engine, std::string_view path) {
    return effect_viewer_.open(picture_engine(engine), path, message_);
}

bool Editor::edit_prefab(app::Engine& engine, std::string_view path) {
    return prefab_editor_.open(picture_engine(engine), path, message_);
}

bool Editor::new_prefab(app::Engine& engine, std::string_view path) {
    return prefab_editor_.open_new(picture_engine(engine), path, message_);
}

void Editor::edit_prefab_command(void* user, const console::Args& args, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    if (args.size() != 1) {
        out.error("editor.edit_prefab takes a prefab's src:/ path");
        return;
    }
    if (!editor->edit_prefab(*editor->engine_, args[0])) {
        out.error("%s", editor->message_);
        return;
    }
    out.info("editing %.*s", static_cast<int>(args[0].size()), args[0].data());
}

void Editor::new_prefab_command(void* user, const console::Args& args, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    if (args.size() != 1) {
        out.error("editor.new_prefab takes the src:/ path the prefab will have");
        return;
    }
    if (!editor->new_prefab(*editor->engine_, args[0])) {
        out.error("%s", editor->message_);
        return;
    }
    out.info("a new prefab, %.*s: it's a file once it's saved", static_cast<int>(args[0].size()), args[0].data());
}

void Editor::prefab_command(void* user, const console::Args& args, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    PrefabEditor& prefab = editor->prefab_editor_;
    if (!prefab.is_open()) {
        out.error("no prefab is open in the prefab editor: editor.edit_prefab PATH opens one");
        return;
    }
    const PictureEngine engine = picture_engine(*editor->engine_);
    const std::string_view verb = args.empty() ? std::string_view() : args[0];
    // Three numbers from an argument on.
    const auto numbers = [&args](size_t from, Vec3& value) {
        return args.size() == from + 3 && console::Args::parse_float(args[from], value.x) &&
               console::Args::parse_float(args[from + 1], value.y) &&
               console::Args::parse_float(args[from + 2], value.z);
    };
    const auto part_of = [&](std::string_view id) -> const PrefabDocPart* {
        const uint32_t index = prefab.document().find_part(id);
        return index == UINT32_MAX ? nullptr : &prefab.document().parts[index];
    };
    bool done = true;
    bool known = true;
    Vec3 value(0.0f);
    bool on = false;
    if (verb.empty()) {
        // Only what it holds, below.
    } else if (verb == "add" && (args.size() == 2 || numbers(2, value))) {
        done = prefab.add_part(args[1], value);
    } else if (verb == "remove" && args.size() == 2) {
        done = prefab.remove_part(args[1]);
    } else if (verb == "duplicate" && args.size() == 2) {
        done = prefab.duplicate_part(args[1]);
    } else if (verb == "rename" && args.size() == 3) {
        done = prefab.rename_part(args[1], args[2]);
    } else if ((verb == "move" || verb == "rotate" || verb == "scale") && numbers(2, value)) {
        const PrefabDocPart* part = part_of(args[1]);
        done = part != nullptr ? prefab.place_part(args[1], verb == "move" ? value : part->position,
                                                   verb == "rotate" ? value : part->rotation_deg,
                                                   verb == "scale" ? value : part->scale)
                               : prefab.select(args[1]); // says there's none
    } else if (verb == "light" && args.size() == 3 && args[2] == "off") {
        done = prefab.set_light(args[1], nullptr);
    } else if (verb == "light" && (args.size() == 4 || args.size() == 7)) {
        // Its intensity and radius, and its colour if given; the rest as the part's light has it, or a light's own.
        const PrefabDocPart* part = part_of(args[1]);
        asset::LightDesc light = part != nullptr && part->has_light ? part->light : asset::LightDesc{};
        const bool read = console::Args::parse_float(args[2], light.intensity) &&
                          console::Args::parse_float(args[3], light.radius_m) &&
                          (args.size() == 4 || (console::Args::parse_float(args[4], light.color[0]) &&
                                                console::Args::parse_float(args[5], light.color[1]) &&
                                                console::Args::parse_float(args[6], light.color[2])));
        if (!read) {
            out.error("editor.prefab light takes ID off, or ID INTENSITY RADIUS_M [R G B]");
            return;
        }
        done = prefab.set_light(args[1], &light);
    } else if (verb == "select" && args.size() <= 2) {
        done = prefab.select(args.size() == 2 ? args[1] : std::string_view());
    } else if (verb == "variant" && args.size() == 2) {
        done = prefab.add_variant(args[1]);
    } else if (verb == "delete_variant" && args.size() == 2) {
        done = prefab.remove_variant(args[1]);
    } else if (verb == "keep" && args.size() == 4 && console::Args::parse_bool(args[3], on)) {
        done = prefab.leave_out(args[1], args[2], !on);
    } else if (verb == "show" && args.size() <= 2) {
        done = prefab.show_variant(args.size() == 2 ? args[1] : std::string_view());
    } else if (verb == "undo" && args.size() == 1) {
        done = prefab.undo();
    } else if (verb == "redo" && args.size() == 1) {
        done = prefab.redo();
    } else if (verb == "save" && args.size() == 1) {
        done = prefab.save(engine);
    } else if (verb == "frame" && args.size() == 1) {
        prefab.frame(true);
    } else if (verb == "close" && args.size() == 1) {
        prefab.close(engine); // as it is: what wasn't saved goes
        out.info("closed");
        return;
    } else {
        known = false;
    }
    if (!known) {
        out.error("editor.prefab takes add, remove, duplicate, rename, move, rotate, scale, select, variant, "
                  "delete_variant, keep, show, undo, redo, save, frame or close: help editor.prefab says how");
        return;
    }
    if (!done) {
        out.error("%s", prefab.message()[0] != '\0' ? prefab.message() : "nothing to do");
        return;
    }
    const PrefabDocument& document = prefab.document();
    const std::string_view path = prefab.path();
    const std::string_view chosen = prefab.selected();
    out.info("%.*s%s: %zu parts, %zu variants, %zu problems; chosen: %.*s", static_cast<int>(path.size()), path.data(),
             prefab.dirty() ? " (changed, not saved)" : "", document.parts.size(), document.variants.size(),
             prefab.problems().size(), static_cast<int>(chosen.size()), chosen.empty() ? "" : chosen.data());
    for (const PrefabDocPart& part : document.parts) {
        const char* made = !part.prefab.empty() ? part.prefab.c_str()
                           : !part.mesh.empty() ? part.mesh.c_str()
                                                : part.effect.c_str();
        out.info("  %s: %s%s%s%s at %.4g %.4g %.4g, turned %.4g %.4g %.4g, scaled %.4g %.4g %.4g", part.id.c_str(),
                 made, !part.mesh.empty() && !part.effect.empty() ? " + " : "",
                 !part.mesh.empty() && !part.effect.empty() ? part.effect.c_str() : "",
                 part.has_light ? (made[0] != '\0' ? " + a light" : "a light") : "",
                 static_cast<double>(part.position.x), static_cast<double>(part.position.y),
                 static_cast<double>(part.position.z), static_cast<double>(part.rotation_deg.x),
                 static_cast<double>(part.rotation_deg.y), static_cast<double>(part.rotation_deg.z),
                 static_cast<double>(part.scale.x), static_cast<double>(part.scale.y),
                 static_cast<double>(part.scale.z));
    }
    for (const core::FixedString<159>& problem : prefab.problems()) {
        out.info("  problem: %s", problem.c_str());
    }
}

void Editor::effect_command(void* user, const console::Args& args, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    EffectViewer& viewer = editor->effect_viewer_;
    if (!viewer.is_open()) {
        out.error("no effect is open in the viewer: editor.view_effect PATH opens one");
        return;
    }
    EffectPlayback& playback = viewer.playback();
    const std::string_view verb = args.empty() ? std::string_view() : args[0];
    float number = 0.0f;
    bool on = false;
    const bool has_number = args.size() == 2 && console::Args::parse_float(args[1], number);
    if (verb == "play" && args.size() == 1) {
        playback.playing = true;
    } else if (verb == "hold" && args.size() == 1) {
        playback.playing = false;
    } else if (verb == "step" && (args.size() == 1 || has_number)) {
        playback.step(args.size() == 1 ? 1 : static_cast<int>(std::lround(number)));
    } else if (verb == "seek" && has_number) {
        playback.seek(static_cast<double>(number));
    } else if (verb == "speed" && has_number && number > 0.0f) {
        playback.speed = number;
    } else if (verb == "loop" && args.size() == 2 && console::Args::parse_bool(args[1], on)) {
        playback.loop = on;
    } else if (verb == "length" && has_number && number > 0.0f) {
        playback.length_s = static_cast<double>(number);
    } else if (verb == "hide" && has_number && number >= 0.0f) {
        viewer.set_hidden(static_cast<uint32_t>(std::lround(number)));
    } else if (verb == "frame" && args.size() == 1) {
        viewer.frame();
    } else if (verb == "close" && args.size() == 1) {
        viewer.close(picture_engine(*editor->engine_));
        out.info("closed");
        return;
    } else if (!args.empty()) {
        out.error("editor.effect takes play, hold, step N, seek S, speed X, loop on|off, length S, hide MASK, frame "
                  "or close");
        return;
    }
    const std::string_view path = viewer.path();
    out.info("%.*s: %s at %.3f s (step %llu), %.2gx%s, bar %.1f s; %zu emitters, step %.3f ms, draw %.3f ms",
             static_cast<int>(path.size()), path.data(), playback.playing ? "playing" : "held", playback.age_s,
             static_cast<unsigned long long>(playback.step_number()), static_cast<double>(playback.speed),
             playback.loop ? ", looping" : "", playback.length_s, viewer.emitters().size(),
             static_cast<double>(viewer.step_ms()), static_cast<double>(viewer.draw_ms()));
    for (const EmitterRow& row : viewer.emitters()) {
        out.info("  %s: %.4g a second, %u of %u alive%s", row.name.c_str(), static_cast<double>(row.rate_per_s),
                 row.alive, row.capacity, row.held ? "" : " (holds nothing)");
    }
}

void Editor::view_effect_command(void* user, const console::Args& args, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    if (args.size() != 1) {
        out.error("editor.view_effect takes an effect's src:/ path");
        return;
    }
    if (!editor->view_effect(*editor->engine_, args[0])) {
        out.error("%s", editor->message_);
        return;
    }
    out.info("viewing %.*s", static_cast<int>(args[0].size()), args[0].data());
}

PictureEngine Editor::picture_engine(app::Engine& engine) {
    return {.device = &engine.device,
            .shaders = &engine.shaders,
            .assets = &engine.assets,
            .gpu_assets = &engine.gpu_assets,
            .jobs = &engine.jobs,
            .vfs = &engine.vfs};
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
    // The pictures Resources waits for, one at a time; and the effect being looked at.
    pictures_.update(picture_engine(engine), thumbnails_);
    effect_viewer_.update(picture_engine(engine), frame_s, &engine.lighting);
    prefab_editor_.update(picture_engine(engine), frame_s);
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
    if (select_name_ != nullptr && std::string_view(select_name_) == "Terrain" &&
        engine.assets.get<asset::TerrainAsset>(engine.terrain) != nullptr) {
        selection_.select_terrain(); // --select Terrain
        select_name_ = nullptr;
    }
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
    read_folders(engine);
    hierarchy_.draw(engine, selection_, viewer_.camera_entity().id(), commands_);
    DVec3 go_to(0.0);
    if (cell_map_.draw(engine, selection_, go_to)) {
        go_to_place(go_to);
    }
    thumbnails_.update(engine.vfs); // every frame: the properties show pictures too, the drawer open or not
    update_sources(engine);
    const bool open_asked = properties_.draw(engine, selection_, commands_);
    if (const PropertiesRequest asked = properties_.take_request(); asked.kind != PropertiesRequest::Kind::None) {
        switch (asked.kind) {
        case PropertiesRequest::Kind::ShowFile:
            show_in_resources(asked.path.view());
            break;
        case PropertiesRequest::Kind::EditPrefab:
            static_cast<void>(edit_prefab(engine, asked.path.view()));
            break;
        case PropertiesRequest::Kind::ViewEffect:
            static_cast<void>(view_effect(engine, asked.path.view()));
            break;
        case PropertiesRequest::Kind::None:
            break;
        }
    }
    const bool opened = drawer(engine, panels_min, panels_size);
    gather_selection(engine);
    playground(engine);
    effect_viewer_.ui(picture_engine(engine), icons_, kPlayground);
    prefab_editor_.ui(picture_engine(engine), kPlayground,
                      selection_.kind == Selection::Kind::File ? selection_.file.view() : std::string_view());
    // What the prefab editor asks of the editor: a cook after its save (the watch does it by itself with Cook
    // automatically on), a file shown in Resources, the window closed once its unsaved question is answered.
    if (prefab_editor_.take_saved()) {
        browser_.refresh(); // a new prefab's file is there now
        if (!cook_auto_ || !watching_) {
            cook_project(engine);
        }
    }
    if (core::FixedString<255> shown; prefab_editor_.take_show_file(shown)) {
        show_in_resources(shown.view());
    }
    if (core::FixedString<255> made; browser_.take_new_prefab(made)) {
        if (new_prefab(engine, made.view())) {
            drawer_ = Drawer::None; // out of the way of what was asked for
        }
    }
    if (prefab_editor_.take_quit() && may_close(engine)) {
        quit_ = true;
    }
    shortcuts(engine);
    if (show_panel_ != nullptr && ImGui::GetFrameCount() > 2) { // once the panels exist
        ImGui::SetWindowFocus(show_panel_);
        show_panel_ = nullptr;
    }
    if ((open_asked || opened) && selection_.kind == Selection::Kind::File) {
        // A world opens in place of the one shown; an effect in the viewer; a prefab in its editor.
        if (file_kind(selection_.file.view()) == "World") {
            open_world(engine);
        } else if (file_kind(selection_.file.view()) == "Effect") {
            static_cast<void>(view_effect(engine, selection_.file.view()));
        } else if (file_kind(selection_.file.view()) == "Prefab") {
            if (edit_prefab(engine, selection_.file.view())) {
                drawer_ = Drawer::None;
            }
        }
    }
    unsaved_prompt(engine);
    make_prefab_prompt(engine);
    if (selection_.frame_entity) {
        selection_.frame_entity = false;
        frame_selection(engine);
    }
    if (pick_flag_ && picks_waiting_ == 0 && engine.viewport.width > 0 && engine.viewport.height > 0) {
        pick_mode_ = PickMode::Replace;
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
                picked = world::placement_of(entity).id(); // a part of a placed prefab stands for all of it
            }
        });
    }
    if (pick_mode_ == PickMode::Toggle) {
        if (picked != 0) {
            selection_.toggle_entity(picked);
        }
    } else if (pick_mode_ == PickMode::Add) {
        selection_.add_entity(picked);
    } else {
        selection_.select_entity(picked);
    }
}

void Editor::gather_selection(app::Engine& engine) {
    const flecs::world& ecs = engine.world.ecs();
    // Which are chosen, as one number: gathered again when it changes, and a few times a second.
    uint64_t chosen = 0;
    if (selection_.kind == Selection::Kind::Entity) {
        for (const flecs::entity_t id : selection_.entities) {
            chosen = core::mix64(chosen ^ id);
        }
    }
    gathered_s_ += ImGui::GetIO().DeltaTime;
    if (chosen == gathered_for_ && gathered_s_ < 0.25f) {
        return;
    }
    gathered_for_ = chosen;
    gathered_s_ = 0.0f;
    selected_meshes_.clear();
    selected_entities_.clear();
    if (chosen == 0) {
        return;
    }
    // Each, and whatever hangs from it at any depth (flecs::Parent or ChildOf).
    ecs.each([&](flecs::entity entity, const world::MeshInstance& mesh, const world::RenderSlot& slot) {
        flecs::entity at = entity;
        for (uint32_t depth = 0; depth < 64 && at.is_valid(); ++depth) {
            if (selection_.contains(at.id())) {
                selected_meshes_.push({.slot = slot.slot, .mesh = mesh.mesh});
                selected_entities_.push(entity.id());
                return;
            }
            // Up to what it's attached to, or for a part of a placed prefab, to its placement.
            const auto* part = at.try_get<world::PrefabPart>();
            const auto* attached = at.try_get<flecs::Parent>();
            at = part != nullptr       ? ecs.entity(part->placement)
                 : attached != nullptr ? ecs.entity(attached->value)
                                       : at.parent();
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
    // The selection's colliders: its entities and what hangs from them (selected_entities_), each found by physics on
    // itself or its nearest parent.
    collider_entities_.clear();
    if (selection_.kind == Selection::Kind::Entity) {
        collider_entities_.assign(selection_.entities.span());
        for (const flecs::entity_t id : selected_entities_) {
            collider_entities_.push(id);
        }
    }
    engine.collider_entities = collider_entities_.span();
    engine.collider_ground = selection_.kind == Selection::Kind::Terrain;
    engine.editor_overlay.enabled = true;
    engine.editor_overlay.selected = selected_meshes_.span();
    engine.editor_overlay.terrain_selected = selection_.kind == Selection::Kind::Terrain;
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
        world_menu(engine);
        project_menu(engine);
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
        if (ImGui::MenuItem("Add Light")) {
            static_cast<void>(add_light(engine));
        }
        if (ImGui::MenuItem("Make Prefab from Selection...", nullptr, false, an_entity)) {
            ask_make_prefab(engine);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Frame Selected", "F", false,
                            selection_.kind == Selection::Kind::Entity ||
                                selection_.kind == Selection::Kind::Terrain)) {
            selection_.frame_entity = true;
        }
        ImGui::EndMenu();
    }
    prepare_menu_popup();
    if (ImGui::BeginMenu("View")) {
        // The scene's views are the Playground toolbar's.
        ImGui::MenuItem("Frame Stats", "F1", &engine.frame_stats);
        ImGui::MenuItem("Cell Grid", nullptr, &show_cells_);
        ImGui::EndMenu();
    }
    prepare_menu_popup();
    if (ImGui::BeginMenu("Rendering Settings")) {
        ImGui::MenuItem("Temporal Anti-Aliasing", "F3", &engine.taa);
        if (ImGui::BeginMenu("Renderer")) {
            if (ImGui::MenuItem("Rasterized", nullptr, !engine.path_traced)) {
                engine.path_traced = false;
            }
            if (ImGui::MenuItem("Path Traced", nullptr, engine.path_traced)) {
                engine.path_traced = true;
            }
            // 0: direct light alone, for checking the rasterized direct light against it.
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
            ImGui::SliderInt("Bounces", &engine.path_tracer_bounces, 0, 64);
            ImGui::Separator();
            ImGui::MenuItem("Compare with Path Traced", nullptr, &engine.lighting_compare, !engine.path_traced);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::MenuItem("Light Boxes", nullptr, &engine.light_boxes);
        ImGui::MenuItem("Ambient Occlusion", nullptr, &engine.ambient_occlusion);
        ImGui::MenuItem("Radiance Cascades", nullptr, &engine.radiance_cascades);
        ImGui::MenuItem("Auto Exposure", nullptr, &engine.auto_exposure);
#if DE_BUILD_DEV
#endif
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
        menu_section("Frame Rate Limit");
        for (const uint32_t limit : kFrameRateLimits) {
            char label[16] = "None";
            if (limit != 0) {
                static_cast<void>(std::snprintf(label, sizeof(label), "%u fps", limit));
            }
            if (ImGui::MenuItem(label, nullptr, engine.fps_cap == limit)) {
                engine.fps_cap = limit;
                fps_cap_kept_ = limit;
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
        if (engine.frame_debugger != nullptr) {
            const bool open = engine.frame_debugger->open();
            if (ImGui::MenuItem("Frame Debugger", "F8", open)) {
                engine.frame_debugger->set_open(!open);
            }
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
        const float frame_y = ImGui::GetCursorPosY();
        const float label_y = frame_y + ((ImGui::GetFrameHeight() - ImGui::GetFontSize()) * 0.5f);
        const float label_x = ImGui::GetCursorPosX();
        ImGui::SetCursorPosY(label_y);
        const bool typing = engine.console_ui != nullptr && engine.console_ui->command_line_active();
        ImGui::TextColored(color(typing ? ThemeColor::Accent : ThemeColor::TextSecondary), "Cmd");
        ImGui::SameLine(label_x + ImGui::CalcTextSize("Cmd").x + px(6.0f));
        ImGui::SetCursorPosY(frame_y);
        if (engine.console_ui != nullptr) {
            engine.console_ui->draw_command_line(px(380.0f), "a console command (help lists them)");
        }
        // What the focused panel reports, or the editor's last word. (The camera's speed is the toolbar's.)
        const char* text = hierarchy_.focused()           ? hierarchy_.status()
                           : cell_map_.focused()          ? cell_map_.status()
                           : drawer_ == Drawer::Resources ? browser_.status()
                                                          : message_;
        // After a framed item, text goes down by the frame's padding on its own: from the frame's top, not the label's.
        ImGui::SameLine(0.0f, px(14.0f));
        ImGui::SetCursorPosY(frame_y);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(color(ThemeColor::TextSecondary), "%s", text);
        cook_controls(engine, frame_y);
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
                if (browser_.take_files_changed()) {
                    static_cast<void>(cell_sources_.scan(engine.vfs)); // cells moved or renamed: their sources anew
                }
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
        ImGui::SameLine(0.0f, px(8.0f));
        if (icon_button(icons_, "##snap_ground", Icon::SnapTerrain,
                        "Keep what's moved on the ground (the terrain, or what's below it)", snap_ground_)) {
            snap_ground_ = !snap_ground_;
        }
        // Physics' colliders: all near the camera, or the selection's.
        ImGui::SameLine(0.0f, px(8.0f));
        if (icon_button(icons_, "##colliders", Icon::Colliders,
                        "Show the colliders near the camera (physics.show_colliders)", engine.show_colliders)) {
            engine.show_colliders = !engine.show_colliders;
        }
        ImGui::SameLine();
        if (icon_button(icons_, "##colliders_selected", Icon::CollidersSelected,
                        "Show the selection's colliders (the terrain's near the camera, if it's chosen)",
                        engine.show_selected_colliders)) {
            engine.show_selected_colliders = !engine.show_selected_colliders;
        }

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
                        selection_.kind == Selection::Kind::Entity || selection_.kind == Selection::Kind::Terrain)) {
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
        // A model dragged from the resources: shown under the mouse while it's over the scene, placed where it's let
        // go.
        preview_model(engine, playground_view(corner, size),
                      ImGui::IsMouseHoveringRect(corner, far_corner) &&
                          ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem),
                      preview_flag_ != nullptr ? ImVec2(corner.x + (size.x * 0.5f), corner.y + (size.y * 0.5f))
                                               : io.MousePos);
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
        char notice_text[64];
        if (const char* notice = app::viewport_notice(engine, notice_text); notice != nullptr) {
            tools::draw_viewport_notice(draw, corner.x, corner.y, size.x, notice);
        }
        engine.viewport.overlays_drawn = true;
        draw->PushClipRect(corner, far_corner, true);
        if (show_cells_) {
            draw_cell_grid(engine, view, draw);
        }
        draw_light_reach(engine, view, draw);
        // Alt keeps the mouse for the camera: no handle takes it then.
        const bool mouse_free = playground_hovered_ && !looking_ && !io.KeyAlt && !navigation_.dragging();
        gizmo_.set_ground(snap_ground_ ? this : nullptr);
        const bool on_gizmo = gizmo_.update(view, draw, tool_, space_, snap_, selected, selection_.entities.span(),
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
                pick_mode_ = io.KeyCtrl ? PickMode::Toggle : io.KeyShift ? PickMode::Add : PickMode::Replace;
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
    // Camera bookmarks: Ctrl+1 to 9 keeps one, 1 to 9 goes there (not while typing).
    if (!ImGui::GetIO().WantTextInput) {
        for (uint32_t slot = 1; slot <= 9; ++slot) {
            const auto key = static_cast<ImGuiKey>(ImGuiKey_0 + static_cast<int>(slot));
            if (ImGui::Shortcut(static_cast<ImGuiKeyChord>(ImGuiMod_Ctrl) | static_cast<ImGuiKeyChord>(key),
                                ImGuiInputFlags_RouteGlobal)) {
                keep_bookmark(slot);
                save_bookmarks(engine.vfs);
            } else if (ImGui::Shortcut(key, ImGuiInputFlags_RouteGlobal)) {
                static_cast<void>(go_to_bookmark(slot));
            }
        }
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal)) {
        save(engine);
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_B, ImGuiInputFlags_RouteGlobal)) {
        cook_project(engine);
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, ImGuiInputFlags_RouteGlobal)) {
        duplicate_selection(engine);
    }
    // Delete in Resources is the files' (it asks first).
    if (!ImGui::GetIO().WantTextInput && !browser_.focused() &&
        ImGui::Shortcut(ImGuiKey_Delete, ImGuiInputFlags_RouteGlobal)) {
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
        (selection_.kind == Selection::Kind::Entity || selection_.kind == Selection::Kind::Terrain)) {
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

void Editor::start_cook(app::Engine& engine, bool whole_project) {
    if ((cooking_.empty() && !whole_project) || cooker_.running()) {
        return;
    }
    // In the background, the open project into the cooked folder the engine reads: everything in it that has changed
    // (the shaders stay as they are). A save's cook is judged by what the save wrote, the whole project's by all of it.
    const core::Result<platform::PathString> cooker = platform::program_beside_engine("cooker");
    const char* src_root = engine.vfs.mount_root(platform::Mount::Src);
    const char* game_root = engine.vfs.mount_root(platform::Mount::Game);
    if (cooker && src_root != nullptr && game_root != nullptr) {
        const char* args[] = {cooker->c_str(), "--project", src_root, "--out", game_root, "--no-shaders"};
        if (core::Result<platform::Process> started = platform::Process::start(args)) {
            cooker_ = std::move(*started);
            cook_whole_ = whole_project;
            // A save has just written files the watch hasn't seen: what they were before says nothing.
            cook_start_stamp_ = whole_project ? watch_.stamp() : 0;
        } else {
            DE_LOG_ERROR("editor", "the cooker didn't start: %s", started.error().message);
        }
    } else {
        DE_LOG_ERROR("editor", "no cooker beside the engine, or no project open: nothing cooked");
    }
    if (!cooker_.running()) {
        cook_failed_ = true;
        static_cast<void>(std::snprintf(cook_state_, sizeof(cook_state_), "The cooker didn't start: see Output"));
    }
}

const char* Editor::cook_project(app::Engine& engine) {
    if (cooker_.running()) {
        return "Already cooking";
    }
    start_cook(engine, true);
    return cooker_.running() ? "Cooking the project" : "The cooker didn't start: the Output says why";
}

void Editor::cook_command(void* user, const console::Args& /*args*/, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    const char* said = editor->cook_project(*editor->engine_);
    if (editor->cooker_.running()) {
        out.info("%s", said);
    } else {
        out.error("%s", said);
    }
}

void Editor::watch_project(app::Engine& engine) {
    if (!watching_ || engine.vfs.mount_root(platform::Mount::Src) == nullptr) {
        return;
    }
    watch_.update(engine.vfs, engine.jobs, ImGui::GetIO().DeltaTime);
    if (cook_marks_due_ && watch_.ended() >= cook_marks_look_) {
        cook_marks_due_ = false;
        // What the files are now is what's cooked. Unless they changed while it cooked (the cooker's own new metas, or
        // someone's edit): with the switch on, what it started from stands, and one more cook settles it.
        const uint64_t now = watch_.stamp();
        cooked_stamp_ = cook_auto_ && cook_start_stamp_ != 0 && now != cook_start_stamp_ ? cook_start_stamp_ : now;
    }
    if (cook_auto_ && !cooker_.running() && !cook_marks_due_ && watch_.settled() && watch_.stamp() != cooked_stamp_) {
        start_cook(engine, true);
        if (!cooker_.running()) {
            cooked_stamp_ = watch_.stamp(); // it can't be started (said in the Output): not again every frame
        }
    }
}

void Editor::report_cook(app::Engine& engine, int exit_code) {
    const auto report = engine.vfs.read_whole_file("game:/cook_report.json", editor_heap());
    if (!report) {
        cook_failed_ = true;
        static_cast<void>(std::snprintf(cook_state_, sizeof(cook_state_), "Cook failed (exit %d)", exit_code));
        DE_LOG_ERROR("cook", "the cooker ended with %d and left no report (game:/cook_report.json)", exit_code);
        return;
    }
    const std::string_view text(reinterpret_cast<const char*>(report->data()), report->size());
    // The report's own layout: "cooked": N in its stats, then a severity, a path and a message per finding.
    const auto number_after = [&text](std::string_view key) {
        const size_t at = text.find(key);
        return at == std::string_view::npos ? 0U
                                            : static_cast<uint32_t>(to_number(text.substr(at + key.size(), 12), 0.0f));
    };
    const auto value_after = [&text](size_t from, size_t until, std::string_view key) {
        const size_t at = text.find(key, from);
        if (at == std::string_view::npos || at > until) {
            return std::string_view();
        }
        const size_t begin = at + key.size();
        const size_t close = text.find('"', begin);
        return close == std::string_view::npos ? std::string_view() : text.substr(begin, close - begin);
    };
    const uint32_t cooked = number_after(R"("cooked": )");
    constexpr std::string_view kError = R"("severity": "error")";
    uint32_t errors = 0;
    for (size_t at = text.find(kError); at != std::string_view::npos; at = text.find(kError, at + 1)) {
        const size_t end = text.find('}', at);
        const std::string_view path = value_after(at, end, R"("path": ")");
        const std::string_view message = value_after(at, end, R"("message": ")");
        DE_LOG_ERROR("cook", "%.*s: %.*s", static_cast<int>(path.size()), path.data(), static_cast<int>(message.size()),
                     message.data());
        ++errors;
    }
    cook_failed_ = errors > 0 || exit_code != 0;
    if (errors > 0) {
        static_cast<void>(std::snprintf(cook_state_, sizeof(cook_state_), "Cooked %u, %u %s: see Output", cooked,
                                        errors, errors == 1 ? "error" : "errors"));
    } else if (exit_code != 0) {
        static_cast<void>(std::snprintf(cook_state_, sizeof(cook_state_), "Cook failed (exit %d)", exit_code));
    } else if (cooked > 0) {
        static_cast<void>(
            std::snprintf(cook_state_, sizeof(cook_state_), "Cooked %u %s", cooked, cooked == 1 ? "file" : "files"));
    } else {
        static_cast<void>(std::snprintf(cook_state_, sizeof(cook_state_), "Cooked: nothing had changed"));
    }
    DE_LOG_INFO("cook", "%s", cook_state_);
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
    if (record.effect.is_valid()) {
        entity.set(world::EffectInstance{.effect = hold({.id = record.effect, .type = asset::AssetType::Effect})});
    }
    if (record.prefab.is_valid()) {
        // Its parts are made once the prefab is loaded: by the next frame if it already is.
        entity.set(world::PrefabInstance{.prefab = hold({.id = record.prefab, .type = asset::AssetType::Prefab}),
                                         .variant = record.variant});
        entity.add<world::PrefabPending>();
    }
    if (record.has_fog) {
        entity.set(record.fog);
    }
    if (record.has_light) {
        entity.set(record.light);
    }
    if (record.has_water) {
        entity.set(record.water);
    }
    // In its folder; one the editor made has nothing filed yet.
    EditorFolder folder = record.folder;
    if (!member) {
        folder.filed[0] = 0;
    }
    entity.set(folder);
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
    if (const auto* effect = entity.try_get<world::EffectInstance>(); effect != nullptr && effect->effect.is_valid()) {
        out.effect = assets.asset(effect->effect).id;
    }
    if (const auto* placed = entity.try_get<world::PrefabInstance>(); placed != nullptr && placed->prefab.is_valid()) {
        out.prefab = assets.asset(placed->prefab).id;
        out.variant = placed->variant;
    }
    if (const auto* fog = entity.try_get<world::FogVolume>()) {
        out.has_fog = true;
        out.fog = *fog;
    }
    if (const auto* light = entity.try_get<world::LocalLight>()) {
        out.has_light = true;
        out.light = *light;
    }
    if (const auto* water = entity.try_get<world::WaterBody>()) {
        out.has_water = true;
        out.water = *water;
    }
    out.is_static = entity.has<world::StaticTransform>();
    if (const auto* folder = entity.try_get<EditorFolder>()) {
        out.folder = *folder;
    }
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
    selection_.remove_entity(entity.id());
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

void Editor::keep_living_selection(app::Engine& engine) {
    // What an undo or redo made is what's chosen now; what it took away isn't.
    if (!commands_.made().empty()) {
        selection_.select_entity(0);
        for (const flecs::entity_t made : commands_.made()) {
            selection_.add_entity(made);
        }
        return;
    }
    for (size_t i = selection_.entities.size(); i-- > 0;) {
        if (!engine.world.ecs().is_alive(selection_.entities[i])) {
            selection_.remove_entity(selection_.entities[i]);
        }
    }
}

void Editor::undo(app::Engine& engine) {
    static_cast<void>(commands_.undo(engine.world.ecs()));
    keep_living_selection(engine);
}

void Editor::redo(app::Engine& engine) {
    static_cast<void>(commands_.redo(engine.world.ecs()));
    keep_living_selection(engine);
}

bool Editor::editable(flecs::entity entity) const {
    return entity.is_valid() && entity.is_alive() && entity.has<world::Transform>() && !entity.has<world::Cell>() &&
           entity.id() != viewer_.camera_entity().id(); // what's placed: not a cell, not the editor's camera
}

void Editor::chosen_roots(const app::Engine& engine, core::Array<flecs::entity_t>& out) const {
    // Each chosen that's placed, but not what hangs on another chosen one (it goes with that).
    out.clear();
    if (selection_.kind != Selection::Kind::Entity) {
        return;
    }
    const flecs::world& ecs = engine.world.ecs();
    for (const flecs::entity_t id : selection_.entities) {
        if (!ecs.is_alive(id) || !editable(ecs.entity(id))) {
            continue;
        }
        bool follows = false;
        for (const auto* parent = ecs.entity(id).try_get<flecs::Parent>(); parent != nullptr && !follows;) {
            follows = selection_.contains(parent->value);
            const flecs::entity up = ecs.entity(parent->value);
            parent = up.is_alive() ? up.try_get<flecs::Parent>() : nullptr;
        }
        if (!follows) {
            out.push(id);
        }
    }
}

void Editor::delete_selection(app::Engine& engine) {
    core::Array<flecs::entity_t> roots(editor_heap());
    chosen_roots(engine, roots);
    if (roots.empty()) {
        return;
    }
    char label[128];
    entity_label(engine.assets, engine.world.ecs().entity(roots[0]), label);
    uint32_t deleted = 0;
    commands_.begin_group();
    for (const flecs::entity_t id : roots) {
        if (engine.world.ecs().is_alive(id) && commands_.remove(engine.world.ecs().entity(id))) {
            ++deleted;
        }
    }
    commands_.end_group();
    if (deleted == 1) {
        static_cast<void>(std::snprintf(message_, sizeof(message_), "Deleted %s (Ctrl+Z puts it back)", label));
    } else if (deleted > 1) {
        static_cast<void>(std::snprintf(message_, sizeof(message_), "Deleted %u (Ctrl+Z puts them back)", deleted));
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
    core::Array<flecs::entity_t> roots(editor_heap());
    chosen_roots(engine, roots);
    uint32_t dropped = 0;
    commands_.begin_group();
    for (const flecs::entity_t id : roots) {
        dropped += drop_to_ground(engine, engine.world.ecs().entity(id)) ? 1U : 0U;
    }
    commands_.end_group();
    if (dropped > 1) {
        static_cast<void>(std::snprintf(message_, sizeof(message_), "Dropped %u to the ground", dropped));
    }
    return dropped > 0;
}

bool Editor::settle(flecs::entity entity, world::WorldTransform& where) {
    app::Engine& engine = *engine_;
    // Its lowest and highest points, as it's turned and scaled: the box round its mesh (its pivot without one).
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
            const double height = (where.rotation * (where.scale * corner)).y;
            lowest = std::min(lowest, height);
            highest = std::max(highest, height);
        }
    }
    // Looked for from just over its top, so one half sunk into the ground comes up onto it; its lowest point lands.
    DVec3 ground;
    bool below = ground_below(engine, where.position + DVec3(0.0, highest + 0.5, 0.0), entity, ground);
    // Under the terrain altogether: up onto it.
    if (double terrain = 0.0; !below && terrain_heights_.has_value() &&
                              terrain_heights_->height_at(where.position.x, where.position.z, terrain)) {
        ground = DVec3(where.position.x, terrain, where.position.z);
        below = true;
    }
    if (!below) {
        return false;
    }
    where.position.y = ground.y - lowest;
    return true;
}

bool Editor::drop_to_ground(app::Engine& engine, flecs::entity entity) {
    const flecs::world& ecs = engine.world.ecs();
    const auto* where = editable(entity) ? entity.try_get<world::WorldTransform>() : nullptr;
    if (where == nullptr) {
        return false;
    }
    const world::WorldTransform now = *where;
    world::WorldTransform dropped = now;
    if (!settle(entity, dropped)) {
        static_cast<void>(std::snprintf(message_, sizeof(message_), "Nothing below it to drop onto"));
        return false;
    }
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
    core::Array<flecs::entity_t> roots(editor_heap());
    chosen_roots(engine, roots);
    if (roots.empty()) {
        return;
    }
    core::Array<flecs::entity_t> copies(editor_heap());
    commands_.begin_group();
    for (const flecs::entity_t id : roots) {
        if (const flecs::entity made = duplicate_entity(engine, engine.world.ecs().entity(id)); made.is_valid()) {
            copies.push(made.id());
        }
    }
    commands_.end_group();
    if (copies.empty()) {
        return;
    }
    selection_.select_entity(0);
    for (const flecs::entity_t made : copies) {
        selection_.add_entity(made);
    }
    if (copies.size() > 1) {
        static_cast<void>(
            std::snprintf(message_, sizeof(message_), "Duplicated %zu: move them where they go", copies.size()));
    }
}

flecs::entity Editor::duplicate_entity(app::Engine& engine, flecs::entity source) {
    if (!editable(source)) {
        return {};
    }
    EntityRecord copy;
    record(source, copy);
    if (!copy.has_entry) {
        static_cast<void>(std::snprintf(message_, sizeof(message_),
                                        "Can't duplicate it: its cell's file wasn't found (cook, then try again)"));
        return {};
    }
    copy.cell = 0; // a new one: made in the editor, saved into the cell it stands in
    char name[128];
    unique_name(engine, copy.entry.name, name);
    static_cast<void>(rename_entry(copy.entry, name));
    const flecs::entity made = make(copy);
    commands_.created(made);
    static_cast<void>(
        std::snprintf(message_, sizeof(message_), "Duplicated %s: move it where it goes", copy.entry.name));
    return made;
}

DVec3 Editor::drop_point(app::Engine& engine, const View& view, ImVec2 screen) {
    // Where the ray under the mouse meets the ground: the nearer of something solid and the terrain, else the level of
    // the world's zero. (A model being previewed has no collision yet: the ray goes through it.)
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
    if (!found && direction.y < -1e-4f) {
        const double along = -view.position.y / static_cast<double>(direction.y);
        if (along > 0.0 && along < kReach) {
            point = view.position + (DVec3(direction) * along);
        }
    }
    return point;
}

void Editor::place_model(app::Engine& engine, std::string_view path, const View& view, ImVec2 screen) {
    const DVec3 point = drop_point(engine, view, screen);
    // The preview, where it is: it's the one placed.
    const flecs::world& ecs = engine.world.ecs();
    if (preview_ != 0 && ecs.is_alive(preview_) && preview_path_.view() == path) {
        const flecs::entity placed = ecs.entity(preview_);
        world::place(placed, {.position = point});
        preview_ = 0;
        commands_.created(placed);
        selection_.select_entity(placed.id());
        const auto* entry = placed.try_get<EditorEntry>();
        static_cast<void>(std::snprintf(message_, sizeof(message_), "Placed %s (saved into its cell with Ctrl+S)",
                                        entry != nullptr ? entry->name : "it"));
        return;
    }
    end_preview(engine);
    static_cast<void>(place_model_at(engine, path, point));
}

void Editor::preview_model(app::Engine& engine, const View& view, bool hovering, ImVec2 screen) {
    const ImGuiPayload* drag = ImGui::GetDragDropPayload();
    const bool dragging = drag != nullptr && drag->IsDataType(AssetBrowser::kSourceFilePayload);
    if ((!dragging || !hovering) && preview_flag_ == nullptr) {
        end_preview(engine);
        return;
    }
    const std::string_view path = dragging ? std::string_view(static_cast<const char*>(drag->Data),
                                                              static_cast<size_t>(std::max(drag->DataSize - 1, 0)))
                                           : std::string_view(preview_flag_);
    if (file_kind(path) != "Model" && file_kind(path) != "Effect" && file_kind(path) != "Prefab") {
        end_preview(engine);
        return;
    }
    const DVec3 point = drop_point(engine, view, screen);
    const flecs::world& ecs = engine.world.ecs();
    if (preview_ != 0 && ecs.is_alive(preview_) && preview_path_.view() == path) {
        world::place(ecs.entity(preview_), {.position = point});
        return;
    }
    end_preview(engine);
    EntityRecord record;
    if (!model_record(engine, path, point, record)) {
        return; // not cooked: the drop says so
    }
    preview_ = make(record).id();
    preview_path_.assign(path);
}

void Editor::end_preview(app::Engine& engine) {
    if (preview_ == 0) {
        return;
    }
    const flecs::world& ecs = engine.world.ecs();
    if (ecs.is_alive(preview_)) {
        ecs.entity(preview_).destruct();
    }
    preview_ = 0;
}

bool Editor::model_record(app::Engine& engine, std::string_view path, const DVec3& point, EntityRecord& out) {
    const bool effect = file_kind(path) == "Effect";
    const bool prefab = file_kind(path) == "Prefab";
    if (file_kind(path) != "Model" && !effect && !prefab) {
        static_cast<void>(
            std::snprintf(message_, sizeof(message_),
                          "Only models (.gltf, .glb, .fbx), effects (.vfx) and prefabs (.prefab) can be placed"));
        return false;
    }
    asset::Guid guid;
    asset::AssetId mesh;
    if (!source_asset_id(engine.vfs, path, guid, mesh)) {
        static_cast<void>(std::snprintf(message_, sizeof(message_), "%.*s has no meta: run the cooker first",
                                        static_cast<int>(path.size()), path.data()));
        return false;
    }
    // Its entry: named after its file, the model by its name from its own folder, solid as props are; or the effect
    // it plays or the prefab it places, likewise.
    const size_t slash = path.rfind('/');
    const std::string_view file = path.substr(slash + 1);
    const std::string_view stem = file.substr(0, file.rfind('.'));
    out = EntityRecord{};
    out.transform = {.position = point};
    // The source's own asset: a model's mesh, the effect, or the prefab.
    (prefab ? out.prefab : effect ? out.effect : out.mesh) = mesh;
    out.is_static = true;
    out.has_entry = true;
    static_cast<void>(std::snprintf(out.entry.object, sizeof(out.entry.object),
                                    prefab   ? R"({"name": "%.*s", "prefab": "%.*s"})"
                                    : effect ? R"({"name": "%.*s", "effect": "%.*s"})"
                                             : R"({"name": "%.*s", "mesh": "%.*s", "collision": "mesh"})",
                                    static_cast<int>(stem.size()), stem.data(), static_cast<int>(file.size()),
                                    file.data()));
    const std::string_view folder = path.substr(0, slash);
    const size_t folder_length = std::min(folder.size(), sizeof(out.entry.folder) - 1);
    std::memcpy(out.entry.folder, folder.data(), folder_length);
    out.entry.folder[folder_length] = 0;
    static_cast<void>(
        std::snprintf(out.entry.name, sizeof(out.entry.name), "%.*s", static_cast<int>(stem.size()), stem.data()));
    char name[128];
    unique_name(engine, out.entry.name, name);
    static_cast<void>(rename_entry(out.entry, name));
    return true;
}

bool Editor::add_light(app::Engine& engine, const DVec3* at) {
    // Where: given, or a man's height above where the middle of the Playground meets the ground.
    DVec3 point(0.0);
    if (at != nullptr) {
        point = *at;
    } else {
        const ImVec2 middle(last_view_.min.x + (last_view_.size.x * 0.5f),
                            last_view_.min.y + (last_view_.size.y * 0.5f));
        point = drop_point(engine, last_view_, middle) + DVec3(0.0, 1.8, 0.0);
    }
    EntityRecord placed;
    placed.transform = {.position = point};
    placed.is_static = true;
    placed.has_light = true;
    placed.light = world::LocalLight{.color = {1.0f, 0.78f, 0.55f}, .intensity = 2.0f, .radius_m = 6.0f}; // a warm lamp
    // Its entry: a light and nothing else. The save writes its values as they are then.
    placed.has_entry = true;
    static_cast<void>(
        std::snprintf(placed.entry.object, sizeof(placed.entry.object), R"({"name": "light", "light": {}})"));
    static_cast<void>(std::snprintf(placed.entry.folder, sizeof(placed.entry.folder), "src:/content"));
    static_cast<void>(std::snprintf(placed.entry.name, sizeof(placed.entry.name), "light"));
    char name[128];
    unique_name(engine, placed.entry.name, name);
    static_cast<void>(rename_entry(placed.entry, name));
    const flecs::entity made = make(placed);
    commands_.created(made);
    selection_.select_entity(made.id());
    static_cast<void>(std::snprintf(message_, sizeof(message_),
                                    "Added %s: its colour, intensity and radius are in Properties (saved into its "
                                    "cell with Ctrl+S)",
                                    placed.entry.name));
    return true;
}

void Editor::add_light_command(void* user, const console::Args& args, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    const bool placed = args.size() == 3 && console::Args::parse_float(args[0], x) &&
                        console::Args::parse_float(args[1], y) && console::Args::parse_float(args[2], z);
    if (!args.empty() && !placed) {
        out.error("editor.add_light takes nothing, or a place: x y z in meters");
        return;
    }
    const DVec3 point(static_cast<double>(x), static_cast<double>(y), static_cast<double>(z));
    static_cast<void>(editor->add_light(*editor->engine_, placed ? &point : nullptr));
    out.info("%s", editor->message_);
}

void Editor::draw_light_reach(app::Engine& engine, const View& view, ImDrawList* draw) {
    if (selection_.kind != Selection::Kind::Entity) {
        return;
    }
    const flecs::world& ecs = engine.world.ecs();
    constexpr int kSegments = 48;
    const ImU32 ring = IM_COL32(245, 197, 57, 200); // a warm yellow
    const auto reach_of = [&](flecs::entity entity) {
        const auto* light = entity.try_get<world::LocalLight>();
        const auto* at = entity.try_get<world::WorldTransform>();
        if (light == nullptr || at == nullptr) {
            return;
        }
        const float scale = std::max({std::abs(at->scale.x), std::abs(at->scale.y), std::abs(at->scale.z)});
        const auto reach = static_cast<double>(light->radius_m * scale);
        // A ring about each axis: a sphere's outline from any side.
        for (int axis = 0; axis < 3; ++axis) {
            ImVec2 last;
            bool last_seen = false;
            for (int i = 0; i <= kSegments; ++i) {
                const double angle = glm::two_pi<double>() * static_cast<double>(i) / static_cast<double>(kSegments);
                DVec3 offset(0.0);
                offset[(axis + 1) % 3] = std::cos(angle) * reach;
                offset[(axis + 2) % 3] = std::sin(angle) * reach;
                ImVec2 now;
                const bool seen = view.project(at->position + offset, now);
                if (seen && last_seen) {
                    draw->AddLine(last, now, ring, px(1.0f));
                }
                last = now;
                last_seen = seen;
            }
        }
        // A spot: the way it shines.
        if (light->cone_deg > 0.0f) {
            ImVec2 from;
            ImVec2 to;
            const DVec3 tip = at->position + (DVec3(at->rotation * Vec3(0.0f, 0.0f, -1.0f)) * reach);
            if (view.project(at->position, from) && view.project(tip, to)) {
                draw->AddLine(from, to, ring, px(1.5f));
            }
        }
    };
    for (const flecs::entity_t id : selection_.entities) {
        if (!ecs.is_alive(id)) {
            continue;
        }
        const flecs::entity entity = ecs.entity(id);
        reach_of(entity);
        if (const auto* placed = entity.try_get<world::PrefabInstance>(); placed != nullptr) {
            world::each_prefab_part(ecs, placed->first_part, reach_of);
        }
    }
}

bool Editor::place_model_at(app::Engine& engine, std::string_view path, const DVec3& point) {
    EntityRecord placed;
    if (!model_record(engine, path, point, placed)) {
        return false;
    }
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
        out.error("editor.place takes a model's, an effect's or a prefab's src:/ path and x y z");
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
        out.error("editor.drop takes a model's or an effect's src:/ path, with the Playground shown");
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
    if (!editor->choose_named(args, out)) {
        return;
    }
    editor->duplicate_selection(*editor->engine_);
    out.info("%s", editor->message_);
}

void Editor::draw_cell_grid(app::Engine& engine, const View& view, ImDrawList* draw) {
    DE_PROFILE_ZONE("Editor::draw_cell_grid");
    // The cells round the camera, their borders a little above the ground (the terrain's, else the world's zero), in
    // steps short enough to follow it.
    constexpr int kReach = 3;        // cells each way
    constexpr double kStepM = 8.0;   // along a border
    constexpr double kAboveM = 0.15; // over the ground, so it isn't lost in it
    const double cell = world::kCellSize;
    if (terrain_heights_.has_value()) {
        terrain_heights_->set_terrain(engine.assets.get<asset::TerrainAsset>(engine.terrain));
    }
    const auto ground = [&](double x, double z) {
        double y = 0.0;
        if (!terrain_heights_.has_value() || !terrain_heights_->height_at(x, z, y)) {
            y = 0.0;
        }
        return DVec3(x, y + kAboveM, z);
    };
    const auto at_x = static_cast<int>(std::floor(view.position.x / cell));
    const auto at_z = static_cast<int>(std::floor(view.position.z / cell));
    const ImU32 ink = IM_COL32(255, 255, 255, 70);
    const float width = std::max(1.0f, px(1.0f));
    const auto line = [&](DVec3 from, DVec3 to) {
        ImVec2 a;
        ImVec2 b;
        if (view.project(from, a) && view.project(to, b)) {
            draw->AddLine(a, b, ink, width);
        }
    };
    const double low_x = (at_x - kReach) * cell;
    const double high_x = (at_x + kReach + 1) * cell;
    const double low_z = (at_z - kReach) * cell;
    const double high_z = (at_z + kReach + 1) * cell;
    for (int k = at_x - kReach; k <= at_x + kReach + 1; ++k) {
        const double x = k * cell;
        for (int step = 0; low_z + (step * kStepM) < high_z; ++step) {
            const double z = low_z + (step * kStepM);
            line(ground(x, z), ground(x, std::min(z + kStepM, high_z)));
        }
    }
    for (int k = at_z - kReach; k <= at_z + kReach + 1; ++k) {
        const double z = k * cell;
        for (int step = 0; low_x + (step * kStepM) < high_x; ++step) {
            const double x = low_x + (step * kStepM);
            line(ground(x, z), ground(std::min(x + kStepM, high_x), z));
        }
    }
    // Each cell's coordinates in its middle.
    char label[32];
    for (int cz = at_z - kReach; cz <= at_z + kReach; ++cz) {
        for (int cx = at_x - kReach; cx <= at_x + kReach; ++cx) {
            ImVec2 middle;
            if (!view.project(ground((cx + 0.5) * cell, (cz + 0.5) * cell), middle)) {
                continue;
            }
            const int length = std::snprintf(label, sizeof(label), "[%d, %d]", cx, cz);
            const ImVec2 size = ImGui::CalcTextSize(label, label + std::max(length, 0));
            draw->AddText(ImVec2(std::floor(middle.x - (size.x * 0.5f)), std::floor(middle.y - (size.y * 0.5f))),
                          IM_COL32(255, 255, 255, 120), label);
        }
    }
}

void Editor::keep_bookmark(uint32_t slot) {
    const uint64_t world = viewer_.world_id().value();
    Bookmark* kept = nullptr;
    for (Bookmark& bookmark : bookmarks_) {
        if (bookmark.world == world && bookmark.slot == slot) {
            kept = &bookmark;
        }
    }
    if (kept == nullptr) {
        kept = &bookmarks_.push({.world = world, .slot = slot, .pose = {}});
    }
    kept->pose = camera_pose();
    static_cast<void>(std::snprintf(message_, sizeof(message_), "Bookmark %u kept (%u goes back there)", slot, slot));
}

bool Editor::go_to_bookmark(uint32_t slot) {
    const uint64_t world = viewer_.world_id().value();
    for (const Bookmark& bookmark : bookmarks_) {
        if (bookmark.world == world && bookmark.slot == slot) {
            viewer_.set_camera_pose(bookmark.pose);
            static_cast<void>(std::snprintf(message_, sizeof(message_), "Bookmark %u", slot));
            return true;
        }
    }
    static_cast<void>(
        std::snprintf(message_, sizeof(message_), "No bookmark %u in this world: Ctrl+%u keeps one", slot, slot));
    return false;
}

void Editor::load_bookmarks(const platform::Vfs& vfs) {
    bookmarks_.clear();
    if (!vfs.exists(kBookmarksPath)) {
        return;
    }
    auto file = vfs.read_whole_file(kBookmarksPath, editor_heap());
    if (!file) {
        return;
    }
    // "world slot x y z yaw_deg pitch_deg", a line each.
    std::string_view text = core::skip_bom(std::string_view(reinterpret_cast<const char*>(file->data()), file->size()));
    while (!text.empty()) {
        const std::string_view line = core::trim(core::strip_comment(core::next_line(text)));
        if (line.empty()) {
            continue;
        }
        // Its words, each a number.
        double values[7] = {};
        size_t count = 0;
        uint64_t world = 0;
        std::string_view rest = line;
        bool good = true;
        while (good && !rest.empty() && count < 7) {
            const size_t space = rest.find(' ');
            const std::string_view word = rest.substr(0, space);
            rest = space == std::string_view::npos ? std::string_view() : core::trim(rest.substr(space));
            const char* begin = word.data();
            const char* end = begin + word.size();
            good = count == 0 ? std::from_chars(begin, end, world, 16).ptr == end
                              : std::from_chars(begin, end, values[count]).ptr == end;
            ++count;
        }
        const auto slot = static_cast<uint32_t>(values[1]);
        if (good && count == 7 && slot >= 1 && slot <= 9) {
            constexpr double kRad = std::numbers::pi / 180.0;
            bookmarks_.push({.world = world,
                             .slot = slot,
                             .pose = {.position = DVec3(values[2], values[3], values[4]),
                                      .yaw_rad = static_cast<float>(values[5] * kRad),
                                      .pitch_rad = static_cast<float>(values[6] * kRad)}});
        }
    }
}

void Editor::save_bookmarks(const platform::Vfs& vfs) const {
    core::Array<char> text(editor_heap());
    const auto add = [&text](const char* line, int length) {
        if (length > 0) {
            const size_t at = text.size();
            text.resize(at + static_cast<size_t>(length));
            std::memcpy(text.data() + at, line, static_cast<size_t>(length));
        }
    };
    char line[256];
    add(line, std::snprintf(line, sizeof(line), "# Camera bookmarks: world, slot, x y z (m), yaw and pitch (deg).\n"));
    constexpr double kDeg = 180.0 / std::numbers::pi;
    for (const Bookmark& bookmark : bookmarks_) {
        add(line, std::snprintf(line, sizeof(line), "%016llx %u %.4f %.4f %.4f %.3f %.3f\n",
                                static_cast<unsigned long long>(bookmark.world), bookmark.slot,
                                bookmark.pose.position.x, bookmark.pose.position.y, bookmark.pose.position.z,
                                bookmark.pose.yaw_rad * kDeg, bookmark.pose.pitch_rad * kDeg));
    }
    if (const core::Result<void> written = vfs.write_file(kBookmarksPath, std::as_bytes(text.span())); !written) {
        DE_LOG_WARN("editor", "can't keep the bookmarks in %s: %s", kBookmarksPath, written.error().message);
    }
}

void Editor::bookmark_command(void* user, const console::Args& args, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    const uint32_t slot = args.empty() || args[0].size() != 1 ? 0 : static_cast<uint32_t>(args[0][0] - '0');
    if (slot < 1 || slot > 9 || args.size() > 2 || (args.size() == 2 && args[1] != "set")) {
        out.error("editor.bookmark takes a slot, 1 to 9, and set to keep the camera in it");
        return;
    }
    if (args.size() == 2) {
        editor->keep_bookmark(slot);
        editor->save_bookmarks(editor->engine_->vfs);
    } else if (!editor->go_to_bookmark(slot)) {
        out.error("%s", editor->message_);
        return;
    }
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
    if (!editor->choose_named(args, out)) {
        return;
    }
    if (!editor->drop_selection_to_ground(*editor->engine_)) {
        out.error("%s", editor->message_);
        return;
    }
    const flecs::world& ecs = editor->engine_->world.ecs();
    if (ecs.is_alive(editor->selection_.entity)) {
        const DVec3 at = ecs.entity(editor->selection_.entity).get<world::WorldTransform>().position;
        out.info("%s: at %.3f %.3f %.3f", editor->message_, at.x, at.y, at.z);
    }
}

bool Editor::choose_named(const console::Args& args, console::Output& out) {
    if (args.empty()) {
        return true;
    }
    Selection chosen;
    for (size_t i = 0; i < args.size(); ++i) {
        const flecs::entity found = find_named(*engine_, args[i]);
        if (!found.is_valid()) {
            out.error("nothing is called %.*s", static_cast<int>(args[i].size()), args[i].data());
            return false;
        }
        chosen.add_entity(found.id());
    }
    selection_.select_entity(0);
    for (const flecs::entity_t id : chosen.entities) {
        selection_.add_entity(id);
    }
    return true;
}

void Editor::select_command(void* user, const console::Args& args, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    if (args.empty()) {
        editor->selection_.select_entity(0);
        out.info("nothing chosen");
        return;
    }
    if (args.size() == 1 && args[0] == "Terrain") {
        if (editor->engine_->assets.get<asset::TerrainAsset>(editor->engine_->terrain) == nullptr) {
            out.error("this world has no terrain");
            return;
        }
        editor->selection_.select_terrain();
        out.info("the terrain chosen");
        return;
    }
    if (editor->choose_named(args, out)) {
        out.info("%zu chosen", editor->selection_.entities.size());
    }
}

void Editor::files_changed(app::Engine& engine) {
    browser_.refresh();
    static_cast<void>(cell_sources_.scan(engine.vfs));
}

void Editor::move_file_command(void* user, const console::Args& args, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    if (args.size() != 2) {
        out.error(
            "editor.move_file takes where it is and where it goes: src:/content/a/rock.gltf src:/content/b/rock.gltf");
        return;
    }
    const SourceMove move{.from = SourcePath(args[0]), .to = SourcePath(args[1])};
    const core::Result<FileOpReport> report = move_sources(editor->engine_->vfs, std::span(&move, 1));
    if (!report) {
        out.error("not moved: %s", report.error().message);
        return;
    }
    editor->files_changed(*editor->engine_);
    out.info("moved; %u file%s naming it rewritten", report->rewritten, report->rewritten == 1 ? "" : "s");
}

void Editor::new_folder_command(void* user, const console::Args& args, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    if (args.size() != 1) {
        out.error("editor.new_folder takes a path: src:/content/props/rocks");
        return;
    }
    if (const core::Result<void> made = make_source_folder(editor->engine_->vfs, args[0]); !made) {
        out.error("not made: %s", made.error().message);
        return;
    }
    editor->files_changed(*editor->engine_);
    out.info("made");
}

void Editor::delete_file_command(void* user, const console::Args& args, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    if (args.empty()) {
        out.error("editor.delete_file takes paths: src:/content/props/rock");
        return;
    }
    core::Array<SourcePath> paths(editor_heap());
    core::Array<SourcePath> users(editor_heap());
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string_view path = args[i];
        paths.push(SourcePath(path));
        users.clear();
        find_users(editor->engine_->vfs, path, users);
        for (const SourcePath& named_by : users) {
            out.info("careful: %.*s is still named by %s", static_cast<int>(path.size()), path.data(),
                     named_by.c_str());
        }
    }
    const core::Result<uint32_t> trashed = trash_sources(editor->engine_->vfs, paths.span());
    if (!trashed) {
        out.error("not deleted: %s", trashed.error().message);
        return;
    }
    editor->files_changed(*editor->engine_);
    out.info("%u sent to the recycle bin", *trashed);
}

void Editor::frame_command(void* user, const console::Args& /*args*/, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    if (editor->selection_.kind != Selection::Kind::Entity && editor->selection_.kind != Selection::Kind::Terrain) {
        out.error("nothing chosen to frame");
        return;
    }
    editor->selection_.frame_entity = true;
    out.info("framing the selection");
}

void Editor::delete_command(void* user, const console::Args& args, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    if (!editor->choose_named(args, out)) {
        return;
    }
    editor->delete_selection(*editor->engine_);
    out.info("%s", editor->message_);
}

bool Editor::may_close(app::Engine& /*engine*/) {
    if (!keep_settings_) {
        return true; // a picture's run
    }
    // The prefab open in its editor first, then the world's edits: each asks for itself.
    if (!prefab_editor_.may_quit()) {
        return false;
    }
    if (!unsaved()) {
        return true; // nothing to lose
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
        ImGui::TextUnformatted(pending_ == Pending::Quit          ? "Save your edits before closing?"
                               : pending_ == Pending::OpenProject ? "Save your edits before opening another project?"
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
                } else if (pending == Pending::OpenProject) {
                    open_project(engine, true);
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

void Editor::read_folders(app::Engine& engine) {
    DE_PROFILE_ZONE("Editor::read_folders");
    const flecs::world& ecs = engine.world.ecs();
    // The cells spawned since: their members get the folders their files give them.
    scratch_entities_.clear();
    ecs.each([&](flecs::entity cell, const world::CellContents& contents) {
        if (contents.revision == 0) {
            return;
        }
        if (const uint32_t* read = folders_read_.find(cell.id()); read != nullptr && *read == contents.revision) {
            return;
        }
        folders_read_.set(cell.id(), contents.revision);
        scratch_entities_.push(cell.id());
    });
    if (scratch_entities_.empty()) {
        return;
    }
    struct Filed {
        flecs::entity_t entity;
        EditorFolder folder;
    };
    core::Array<Filed> filed(editor_heap());
    CellSource file(editor_heap());
    for (const flecs::entity_t cell_id : scratch_entities_) {
        const flecs::entity cell = ecs.entity(cell_id);
        const auto* held = cell.try_get<world::Cell>();
        const auto* cooked = held != nullptr ? engine.assets.get<asset::CellAsset>(held->asset) : nullptr;
        if (cooked == nullptr) {
            continue;
        }
        const asset::AssetId id = engine.assets.asset(held->asset).id;
        if (cell_sources_.find(id) == nullptr) {
            static_cast<void>(cell_sources_.scan(engine.vfs)); // a new file, or not looked for yet
        }
        const CellSources::Cell* source = cell_sources_.find(id);
        bool parsed = false;
        if (source != nullptr) {
            if (auto bytes = engine.vfs.read_whole_file(source->path.c_str(), editor_heap())) {
                parsed = file.parse({reinterpret_cast<const char*>(bytes->data()), bytes->size()}).ok();
            }
        }
        const std::span<const asset::CellEntity> entities = cooked->entities.get(cooked);
        ecs.each([&](flecs::entity member, const world::CellMember& at) {
            if (at.index >= entities.size() || member.target<world::InCell>().id() != cell_id) {
                return;
            }
            Filed& entry = filed.push({.entity = member.id(), .folder = {}});
            const std::string_view path =
                parsed ? file.string_member(entities[at.index].source_index, "folder") : std::string_view();
            static_cast<void>(std::snprintf(entry.folder.path, sizeof(entry.folder.path), "%.*s",
                                            static_cast<int>(path.size()), path.data()));
            static_cast<void>(std::snprintf(entry.folder.filed, sizeof(entry.folder.filed), "%s", entry.folder.path));
        });
    }
    // Set after the walk: a component added moves the entity to another table.
    for (const Filed& entry : filed) {
        ecs.entity(entry.entity).set(entry.folder);
    }
}

void Editor::folder_command(void* user, const console::Args& args, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    if (args.empty()) {
        out.error(R"(editor.folder takes a folder ("camp/props", or "" for none) and names, or uses the selection)");
        return;
    }
    const std::string_view path = args[0];
    if (args.size() > 1) {
        Selection chosen;
        for (size_t i = 1; i < args.size(); ++i) {
            const flecs::entity found = editor->find_named(*editor->engine_, args[i]);
            if (!found.is_valid()) {
                out.error("nothing is called %.*s", static_cast<int>(args[i].size()), args[i].data());
                return;
            }
            chosen.add_entity(found.id());
        }
        editor->selection_.select_entity(0);
        for (const flecs::entity_t id : chosen.entities) {
            editor->selection_.add_entity(id);
        }
    }
    core::Array<flecs::entity_t> roots(editor_heap());
    editor->chosen_roots(*editor->engine_, roots);
    HierarchyPanel::move_to_folder(editor->engine_->world.ecs(), editor->commands_, roots.span(), path);
    out.info("%zu into %.*s", roots.size(), static_cast<int>(path.size()), path.data());
}

void Editor::follow_saves(app::Engine& engine) {
    DE_PROFILE_ZONE("Editor::follow_saves");
    int code = 0;
    if (cooker_.running() && cooker_.finished(code)) {
        thumbnails_.forget_all(); // the models cooked come with new pictures
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
        if (cook_whole_) {
            report_cook(engine, code); // the whole project's: by all the report says
        } else if (failed == 0) {
            static_cast<void>(std::snprintf(message_, sizeof(message_), "Saved and cooked"));
            DE_LOG_INFO("editor", "the saved cells cooked%s: hot reload brings them in",
                        code == 0 ? "" : " (something else didn't: the cook report says what)");
        } else {
            static_cast<void>(std::snprintf(message_, sizeof(message_),
                                            "Saved, but %u %s didn't cook: the Output says why", failed,
                                            failed == 1 ? "cell" : "cells"));
        }
        cooker_ = {};
        cook_whole_ = false;
        // What the files are now is what's cooked: the first look to start from here says what that is.
        cook_marks_due_ = true;
        cook_marks_look_ = watch_.started() + 1;
        watch_.look_now();
    }
    watch_project(engine);
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
        selection_.replace_entity(from, to);
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

namespace {
constexpr float kFrameTerrainPitchRad = 0.6f; // about 35 degrees down
} // namespace

void Editor::frame_selection(app::Engine& engine) {
    const flecs::world& ecs = engine.world.ecs();
    const auto* lens = viewer_.camera_entity().try_get<world::Camera>();
    const float fov_rad = lens != nullptr ? lens->vertical_fov_rad : 1.0f;
    // The terrain: all of it, about its middle (its patches' bounds give how high that is).
    if (selection_.kind == Selection::Kind::Terrain) {
        const auto* terrain = engine.assets.get<asset::TerrainAsset>(engine.terrain);
        if (terrain == nullptr) {
            return;
        }
        float lowest = 0.0f;
        float highest = 0.0f;
        bool any = false;
        for (const asset::TerrainPatch& patch : terrain->patches.get(terrain)) {
            lowest = any ? std::min(lowest, patch.min_m) : patch.min_m;
            highest = any ? std::max(highest, patch.max_m) : patch.max_m;
            any = true;
        }
        DVec3 middle(terrain->origin_m[0] + (static_cast<double>(terrain->width_m) * 0.5),
                     (static_cast<double>(lowest) + static_cast<double>(highest)) * 0.5,
                     terrain->origin_m[1] + (static_cast<double>(terrain->depth_m) * 0.5));
        if (terrain_heights_.has_value()) {
            terrain_heights_->set_terrain(terrain);
        }
        if (double ground = 0.0;
            terrain_heights_.has_value() && terrain_heights_->height_at(middle.x, middle.z, ground)) {
            middle.y = ground; // the ground there, nearer than the bounds' middle
        }
        const float radius_m =
            0.5f * std::sqrt((terrain->width_m * terrain->width_m) + (terrain->depth_m * terrain->depth_m));
        // Looked down on, from the way the camera faces: easing back along a level or upward view would go under it.
        tools::CameraPose pose = camera_pose();
        pose.pitch_rad = -kFrameTerrainPitchRad;
        viewer_.set_camera_pose(pose);
        navigation_.frame(pose, middle, std::max(radius_m, 1.0f), fov_rad);
        return;
    }
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
    navigation_.frame(camera_pose(), center, std::max(radius_m, 0.25f), fov_rad);
}

void Editor::cook_controls(app::Engine& engine, float frame_y) {
    // What the cook's doing, or what's waiting on one.
    const bool stale = !cook_auto_ && cooked_stamp_ != 0 && watch_.stamp() != cooked_stamp_;
    const char* state = cooker_.running() ? "Cooking..."
                        : unsaved()       ? "Unsaved edits (Ctrl+S)"
                        : stale           ? "Files changed: not cooked"
                                          : cook_state_;
    const bool warns = !cooker_.running() && (unsaved() || stale || cook_failed_);
    constexpr const char* kAuto = "Cook automatically";
    constexpr const char* kCook = "Cook";
    const float gap = px(6.0f);
    const float state_width = state[0] != '\0' ? ImGui::CalcTextSize(state).x + px(12.0f) : 0.0f;
    const float width = state_width + status_button_width(kAuto) + gap + status_button_width(kCook);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + px(14.0f), ImGui::GetWindowWidth() - width - px(8.0f)));
    if (state[0] != '\0') {
        ImGui::SetCursorPosY(frame_y);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(color(warns ? ThemeColor::Warning : ThemeColor::TextSecondary), "%s", state);
        ImGui::SameLine(0.0f, px(12.0f));
    }
    ImGui::SetCursorPosY(frame_y);
    if (status_button("##cook automatically", kAuto, cook_auto_)) {
        cook_auto_ = !cook_auto_;
    }
    if (ImGui::IsItemHovered()) {
        tooltip("Cook by itself whenever a file of the project changes: a model dropped in, a file edited by hand");
    }
    ImGui::SameLine(0.0f, gap);
    ImGui::SetCursorPosY(frame_y);
    if (status_button("##cook", kCook, false)) {
        cook_project(engine);
    }
    if (ImGui::IsItemHovered()) {
        tooltip("Cook the project now, in the background: what changed shows as it's done (Ctrl+B)");
    }
}

namespace {

constexpr std::string_view kContentRoot = "src:/content/";
constexpr std::string_view kWorldExtension = ".world";

// A world's name: its place under content, without its extension ("terrain/coast").
std::string_view world_name(const platform::PathString& file) {
    std::string_view name = file.view().substr(kContentRoot.size());
    name.remove_suffix(kWorldExtension.size());
    return name;
}

} // namespace

void Editor::list_worlds(app::Engine& engine) {
    // Every .world under src:/content, and the ID the cooker gave each (none: it hasn't seen it).
    world_files_.clear();
    world_ids_.clear();
    core::Array<platform::PathString> folders(editor_heap());
    platform::PathString root;
    root.assign("src:/content");
    folders.push(root);
    while (!folders.empty()) {
        const platform::PathString folder = folders.back();
        folders.pop();
        static_cast<void>(engine.vfs.list(folder.c_str(), [&](const char* name, bool is_directory) {
            platform::PathString path = folder;
            if (!path.append('/') || !path.append(name)) {
                return;
            }
            if (is_directory) {
                folders.push(path);
            } else if (path.view().ends_with(kWorldExtension)) {
                world_files_.push(path);
            }
        }));
    }
    std::ranges::sort(world_files_,
                      [](const platform::PathString& a, const platform::PathString& b) { return a.view() < b.view(); });
    for (const platform::PathString& path : world_files_) {
        asset::Guid guid;
        asset::AssetId id;
        world_ids_.push(source_asset_id(engine.vfs, path.view(), guid, id) ? id : asset::AssetId{});
    }
}

void Editor::list_projects(app::Engine& engine) {
    // Each folder beside the engine's own projects that has its <name>.deproject in it.
    project_names_.clear();
    static_cast<void>(engine.vfs.list("engine:/projects", [&](const char* name, bool is_directory) {
        core::FixedString<63> project;
        platform::PathString marker;
        if (is_directory && project.assign(name) && marker.assign("engine:/projects/") && marker.append(name) &&
            marker.append('/') && marker.append(name) && marker.append(".deproject") &&
            engine.vfs.exists(marker.c_str())) {
            project_names_.push(project);
        }
    }));
    std::ranges::sort(project_names_, [](const core::FixedString<63>& a, const core::FixedString<63>& b) {
        return a.view() < b.view();
    });
}

std::string_view Editor::open_project_name(const app::Engine& engine) {
    const char* root = engine.vfs.mount_root(platform::Mount::Src);
    const std::string_view open = root != nullptr ? std::string_view(root) : std::string_view();
    return open.substr(open.rfind('/') + 1);
}

void Editor::open_world_command(void* user, const console::Args& args, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    app::Engine& engine = *editor->engine_;
    if (args.size() > 1) {
        out.error("editor.open_world takes one world's name (editor.open_world alone lists them)");
        return;
    }
    editor->list_worlds(engine);
    editor->worlds_listed_ = false; // the menu lists again when it next opens
    if (args.empty()) {
        for (size_t i = 0; i < editor->world_files_.size(); ++i) {
            const std::string_view name = world_name(editor->world_files_[i]);
            const asset::AssetId id = editor->world_ids_[i];
            out.info("%.*s%s", static_cast<int>(name.size()), name.data(),
                     !id.is_valid()                     ? "  (new to the cooker: cook the project first)"
                     : id == editor->viewer_.world_id() ? "  (open)"
                                                        : "");
        }
        if (editor->world_files_.empty()) {
            out.info("No worlds in this project");
        }
        return;
    }
    for (size_t i = 0; i < editor->world_files_.size(); ++i) {
        if (world_name(editor->world_files_[i]) != args[0]) {
            continue;
        }
        if (!editor->world_ids_[i].is_valid()) {
            out.error("%s is new to the cooker: cook the project first", editor->world_files_[i].c_str());
        } else if (editor->world_ids_[i] == editor->viewer_.world_id()) {
            out.info("%s is the world that's open", editor->world_files_[i].c_str());
        } else {
            editor->selection_.select_file(editor->world_files_[i].view());
            editor->open_world(engine);
            out.info("%s", editor->pending_ == Pending::OpenWorld ? "Unsaved edits: asked in the editor first"
                                                                  : "Opening it");
        }
        return;
    }
    out.error("No world of that name (editor.open_world alone lists them)");
}

void Editor::open_project_command(void* user, const console::Args& args, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    app::Engine& engine = *editor->engine_;
    if (args.size() > 1) {
        out.error("editor.open_project takes one project's name (editor.open_project alone lists them)");
        return;
    }
    editor->list_projects(engine);
    editor->projects_listed_ = false; // the menu lists again when it next opens
    const std::string_view open = open_project_name(engine);
    if (args.empty()) {
        for (const core::FixedString<63>& name : editor->project_names_) {
            out.info("%s%s", name.c_str(), name.view() == open ? "  (open)" : "");
        }
        if (editor->project_names_.empty()) {
            out.info("No projects beside the engine");
        }
        return;
    }
    for (const core::FixedString<63>& name : editor->project_names_) {
        if (name.view() != args[0]) {
            continue;
        }
        if (name.view() == open) {
            out.info("%s is the project that's open", name.c_str());
            return;
        }
        editor->project_to_open_ = name;
        editor->open_project(engine);
        if (editor->pending_ == Pending::OpenProject) {
            out.info("Unsaved edits: asked in the editor first");
        } else if (editor->quit_) {
            out.info("Opening %s in an editor of its own; this one closes", name.c_str());
        } else {
            out.error("%s", editor->message_);
        }
        return;
    }
    out.error("No project of that name beside the engine (editor.open_project alone lists them)");
}

void Editor::world_menu(app::Engine& engine) {
    if (!ImGui::BeginMenu("Open World")) {
        worlds_listed_ = false;
        return;
    }
    if (!worlds_listed_) {
        worlds_listed_ = true; // once as the menu opens
        list_worlds(engine);
    }
    if (world_files_.empty()) {
        ImGui::MenuItem("No worlds in this project", nullptr, false, false);
    }
    for (size_t i = 0; i < world_files_.size(); ++i) {
        const std::string_view name = world_name(world_files_[i]);
        char label[platform::kMaxPathLength + 1];
        static_cast<void>(std::snprintf(label, sizeof(label), "%.*s", static_cast<int>(name.size()), name.data()));
        const bool cooked = world_ids_[i].is_valid();
        if (ImGui::MenuItem(label, nullptr, cooked && world_ids_[i] == viewer_.world_id(), cooked)) {
            selection_.select_file(world_files_[i].view());
            open_world(engine);
        }
        if (!cooked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            tooltip("New to the cooker: cook the project first");
        }
    }
    ImGui::EndMenu();
}

void Editor::project_menu(app::Engine& engine) {
    if (!ImGui::BeginMenu("Open Project")) {
        projects_listed_ = false;
        return;
    }
    if (!projects_listed_) {
        projects_listed_ = true; // once as the menu opens
        list_projects(engine);
    }
    const std::string_view open = open_project_name(engine); // the one open: its folder's name
    if (project_names_.empty()) {
        ImGui::MenuItem("No projects beside the engine", nullptr, false, false);
    }
    for (const core::FixedString<63>& name : project_names_) {
        const bool is_open = name.view() == open;
        if (ImGui::MenuItem(name.c_str(), nullptr, is_open) && !is_open) {
            project_to_open_ = name;
            open_project(engine);
        }
    }
    ImGui::EndMenu();
}

void Editor::open_project(app::Engine& engine, bool unsaved_asked) {
    if (unsaved() && !unsaved_asked) {
        pending_ = Pending::OpenProject; // asked first; it comes back here
        return;
    }
    // Another editor, on that project, and this one closes: what's mounted can't change while the engine runs.
    const core::Result<platform::PathString> program = platform::program_beside_engine("deengine");
    const char* engine_root = engine.vfs.mount_root(platform::Mount::Engine);
    platform::PathString project;
    char limit[16];
    static_cast<void>(std::snprintf(limit, sizeof(limit), "%u", engine.fps_cap));
    bool started = false;
    if (program && engine_root != nullptr && project.assign(engine_root) && project.append("/projects/") &&
        project.append(project_to_open_.view())) {
        // The settings first: the next editor reads them as it starts, before this one has closed. And this run's
        // frame rate limit goes with it: a flag's isn't among them.
        save_user_settings(engine);
        const char* args[] = {program->c_str(), "--editor", "--project", project.c_str(), "--fps-cap", limit};
        started = platform::Process::start(args).ok();
    }
    if (started) {
        DE_LOG_INFO("editor", "opening project %s in an editor of its own", project_to_open_.c_str());
        quit_ = true; // the cooker, if it's running, finishes on its own
    } else {
        static_cast<void>(std::snprintf(message_, sizeof(message_), "Project %s didn't open: deengine didn't start",
                                        project_to_open_.c_str()));
        DE_LOG_ERROR("editor", "%s", message_);
    }
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
    end_preview(engine);
    commands_.clear(); // their entities go with the world
    release_holds();
    folders_read_.clear();
    saved_changes_ = commands_.changes();
    selection_.entities.clear(); // the file stays selected
    selection_.entity = 0;
    hierarchy_.reset();
    if (viewer_.open_world(engine, id)) {
        static_cast<void>(std::snprintf(message_, sizeof(message_), "Opened %s", selection_.file.c_str()));
        if (watching_) {
            remember_world(engine.vfs, id); // what the next start opens
        }
    } else {
        static_cast<void>(std::snprintf(message_, sizeof(message_), "%s didn't load: is it cooked? (the log says why)",
                                        selection_.file.c_str()));
        DE_LOG_ERROR("editor", "%s", message_);
    }
}

} // namespace de::editor
#endif
