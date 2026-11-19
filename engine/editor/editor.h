#pragma once

#include "app/framework.h"
#include "app/viewer.h"
#include "console/console.h"
#include "editor/asset_browser.h"
#include "editor/cell_map_panel.h"
#include "editor/cell_saver.h"
#include "editor/commands.h"
#include "editor/effect_viewer.h"
#include "editor/entity_record.h"
#include "editor/gizmo.h"
#include "editor/hierarchy_panel.h"
#include "editor/icons.h"
#include "editor/navigation.h"
#include "editor/prefab_editor.h"
#include "editor/preview_pictures.h"
#include "editor/project_watch.h"
#include "editor/properties_panel.h"
#include "editor/selection.h"
#include "editor/thumbnails.h"
#include "platform/process.h"
#include "world/terrain_heights.h"

#include <optional>

#if DE_BUILD_DEV
namespace de::editor {

// deengine --editor, in dev builds (docs/design.md, Editor & tooling): the viewer's world with the editor's panels
// around it, in the user's editor design (theme.h): a header row (the name, the menus), the panels docked with thin
// gaps between them, and a status bar (what the focused panel reports). The hierarchy, with the
// cell map in a tab beside it, sits over the properties on the left; the game's view (the Playground) is the right
// side. Along the bottom, as Unreal's: the Resources drawer (Ctrl+Space), the Output drawer (the engine's
// log and the console's), and a console command line. The frame stats (F1, or View) start hidden. The camera moves as
// in Unity's scene view: the right button held over the Playground flies it (WASD, the mouse to look, the wheel its
// speed), the wheel zooms, a middle drag pans, Alt and a left drag orbits, Alt and a right drag zooms, F frames the
// selection (navigation.h). The Playground's toolbar: undo and redo, the tools (select, move, rotate, scale: Q W E R),
// world or local axes, snapping to a grid, to angles and to scale steps, the camera's speed, the view, framing the
// selection. A click in the scene selects what's under the mouse; the selection's handles move, turn and scale it.
// Ctrl+Z undoes, Ctrl+Y or Ctrl+Shift+Z redoes, Ctrl+S saves the edits into the cells' text sources and cooks them in
// the background (hot reload brings them back), F frames the selected entity, a double-click on a world in the
// resources opens it. Escape doesn't quit; closing the window does. The layout is kept in user:/editor/layout.ini, the
// interface scale, the tool and the snapping in user:/editor/editor.settings. Flags: the viewer's (app/viewer.h), and,
// for pictures of the editor taken from the command line (the agent's eyes):
//   --select NAME   select the entity of that name once it's in the world
//   --select-file PATH   select that source file ("src:/content/terrain/wc.world")
//   --show PANEL    bring that panel's tab to the front ("Cell Map")
//   --pick X,Y      click that pixel of the Playground's scene, once it's drawn: select what's there
//   --tool NAME     select, move, rotate or scale
//   --handles AT    pivot or center
//   --drawer NAME   resources or output: that drawer open
//   --cmd TEXT      that text in the command line, the keyboard on it
//   --cell-grid     the cells' borders shown (View > Cell Grid)
//   --drag H,AMOUNT show a drag under way on handle H (0-2 an axis, 3-5 a plane, 6 the middle), gone AMOUNT (meters,
//                   or radians turning), changing nothing
// A scripted run (a picture) keeps nothing: not the layout, not the settings.
class Editor final : public app::Application, public EntityMaker, public GroundSnap {
public:
    static constexpr const char* kPlayground = "Playground";

    Editor();
    bool startup(app::Engine& engine, const app::FrameworkArgs& args) override;
    bool update(app::Engine& engine, float frame_s) override;
    void ui(app::Engine& engine) override;
    void draw(app::Engine& engine) override;
    void shutdown(app::Engine& engine) override;
    // The window's close button with unsaved edits: asked first (Save, Don't Save, Cancel).
    bool may_close(app::Engine& engine) override;

    // The commands' maker (undo and redo make and take away entities): entity_record.h.
    // Surface snapping (the gizmo's GroundSnap): as End, at where it's dragged to.
    bool settle(flecs::entity entity, world::WorldTransform& where) override;
    flecs::entity make(const EntityRecord& record) override;
    void record(flecs::entity entity, EntityRecord& out) override;
    void unmake(flecs::entity entity) override;

private:
    void header_row(app::Engine& engine);
    void dock_space(ImVec2 size);
    void status_bar(app::Engine& engine, ImVec2 size);
    // The drawer that's open, over the bottom of the panels (min, size: their space): true if a world was asked open.
    bool drawer(app::Engine& engine, ImVec2 min, ImVec2 size);
    // A bottom bar button that opens a drawer, lit while it's open.
    bool drawer_button(const char* id, const char* label, bool content, bool open);
    void playground(app::Engine& engine);
    void playground_toolbar(app::Engine& engine);
    // How the Playground sees the world now (the scene's rectangle on screen).
    View playground_view(ImVec2 min, ImVec2 size) const;
    void shortcuts(app::Engine& engine);
    // The camera, turned as it is, backs off to see the selected entity whole.
    void frame_selection(app::Engine& engine);
    // Where the camera is and how it looks.
    tools::CameraPose camera_pose() const;
    // The camera to a place on the map (X and Z), at its height, looking as it was.
    void go_to_place(const DVec3& place);
    // The world selected in the resources; with unsaved edits, asked first (unsaved_asked: it was).
    void open_world(app::Engine& engine, bool unsaved_asked = false);
    // What waits on the unsaved edits' question, and the question.
    enum class Pending : uint8_t { None, Quit, OpenWorld, OpenProject };
    void unsaved_prompt(app::Engine& engine);
    // Asks the GPU what's at a pixel of the scene's texture; take_pick selects it once the answer is back
    // (Engine::pick_answer).
    void ask_pick(app::Engine& engine, uint32_t x, uint32_t y);
    void take_pick(app::Engine& engine);
    // The selection's meshes: it and what's attached to it, at any depth.
    void gather_selection(app::Engine& engine);
    // Them to the renderer, for its outline.
    void send_selection(app::Engine& engine);
    // Where the handles sit: the selected entity's pivot, or the centre of its geometry (HandlesAt).
    DVec3 handles_origin(const app::Engine& engine, flecs::entity selected) const;
    // File > Save, Ctrl+S: the edits into the cells' text sources (cell_saver.h), then the cooker in the background,
    // so hot reload swaps the cooked cells in.
    void save(app::Engine& engine);
    // View > Cell Grid: the cells' borders on the ground around the camera, each cell's coordinates in its middle.
    void draw_cell_grid(app::Engine& engine, const View& view, ImDrawList* draw);
    // Camera bookmarks: Ctrl+1 to 9 keeps where the camera is, 1 to 9 goes back there; kept per world in
    // user:/editor/bookmarks.settings.
    struct Bookmark {
        uint64_t world = 0;
        uint32_t slot = 0; // 1 to 9
        tools::CameraPose pose;
    };
    void keep_bookmark(uint32_t slot);
    bool go_to_bookmark(uint32_t slot);
    void load_bookmarks(const platform::Vfs& vfs);
    void save_bookmarks(const platform::Vfs& vfs) const;
    // editor.bookmark N [set]: as 1 to 9, or Ctrl with them.
    static void bookmark_command(void* user, const console::Args& args, console::Output& out);
    // Undo and redo, the selection following what they make and take away.
    void undo(app::Engine& engine);
    void redo(app::Engine& engine);
    // Delete: the selected entity away, as a command. Ctrl+D: a copy of it where it stands, made in the editor (saved
    // into its cell at the next save), selected.
    void delete_selection(app::Engine& engine);
    // What an edit of the selection acts on: each chosen that's placed, not what hangs on another chosen (it goes with
    // that). editable: placed (not a cell, not the editor's camera).
    void chosen_roots(const app::Engine& engine, core::Array<flecs::entity_t>& out) const;
    bool editable(flecs::entity entity) const;
    // One of them, for the selection's edits (as a command in their group).
    bool drop_to_ground(app::Engine& engine, flecs::entity entity);
    flecs::entity duplicate_entity(app::Engine& engine, flecs::entity source);
    // After an undo or redo: what it made is chosen; what it took away isn't.
    void keep_living_selection(app::Engine& engine);
    // End: the selection down onto the ground below it, its lowest point (the box round its mesh, as it's turned and
    // scaled) on the nearer of the terrain and something solid (not itself or what hangs on it), as a command. False if
    // there's nothing below it.
    bool drop_selection_to_ground(app::Engine& engine);
    // Where the ground is under start, straight down (start's own height and below): false if nothing is.
    bool ground_below(app::Engine& engine, const DVec3& start, flecs::entity ignore, DVec3& out);
    void duplicate_selection(app::Engine& engine);
    // A model from the resources (src:/ path) dropped on the Playground at screen: made where the ray from the camera
    // meets the ground, as a command, selected.
    void place_model(app::Engine& engine, std::string_view path, const View& view, ImVec2 screen);
    // The same at a place in the world: true if it was placed.
    bool place_model_at(app::Engine& engine, std::string_view path, const DVec3& point);
    // What placing a model makes there (named apart from everything in the world); false (message_ says why) if it
    // isn't a model, or isn't cooked.
    bool model_record(app::Engine& engine, std::string_view path, const DVec3& point, EntityRecord& out);
    // Where a model let go at screen goes: where the ray meets the nearer of something solid and the terrain, else
    // the level of the world's zero.
    DVec3 drop_point(app::Engine& engine, const View& view, ImVec2 screen);
    // While a model is dragged from the resources over the Playground (hovering: the mouse is over its picture), the
    // model itself under the mouse, where it would go; out of the Playground, or the drag over, it goes again.
    void preview_model(app::Engine& engine, const View& view, bool hovering, ImVec2 screen);
    void end_preview(app::Engine& engine);
    // A name no entity in the world has, from base: a trailing number counts up, keeping its width ("boulder_1" to
    // "boulder_5" if 2 to 4 are taken, "rock_01" to "rock_02"), and a name without one gets "_2", "_3"...
    void unique_name(const app::Engine& engine, std::string_view base, std::span<char> out) const;
    // An entry's name, in its object's "name" too.
    static bool rename_entry(EditorEntry& entry, std::string_view name);
    // The first entity with that label, or none.
    flecs::entity find_named(const app::Engine& engine, std::string_view name) const;
    // A cell member's entry in its file, as an entry of its own (no parent: it's a root where it stands): false if its
    // file couldn't be found or read.
    bool entry_from_file(const EntityRecord& record, EditorEntry& out);
    // What the made entities draw with, held while the editor runs (or until another world opens).
    asset::AssetHandle hold(const asset::AssetRef& ref);
    void release_holds();
    // The cooker in the background (unless it's running): over what the last save wrote, or the whole project.
    void start_cook(app::Engine& engine, bool whole_project = false);
    // The whole project cooked, in the background: the status bar's Cook, Ctrl+B, editor.cook.
    const char* cook_project(app::Engine& engine); // what came of asking, for the console
    static void cook_command(void* user, const console::Args& args, console::Output& out);
    // Each frame: the watch over the project's files and, with Cook automatically on, a cook once they've changed.
    void watch_project(app::Engine& engine);
    // What the cooker's report says of a cook of the whole project: the status bar's line, each error in the Output.
    void report_cook(app::Engine& engine, int exit_code);
    // The status bar's right end: what the cook's doing, Cook automatically, Cook.
    void cook_controls(app::Engine& engine, float frame_y);
    // File > Open World: the project's .world files by name. File > Open Project: the projects beside the engine;
    // another one opens in an editor of its own, and this one closes.
    void world_menu(app::Engine& engine);
    void project_menu(app::Engine& engine);
    void open_project(app::Engine& engine, bool unsaved_asked = false);
    // What the two menus list: the project's worlds with the IDs the cooker gave them, and the projects' names.
    void list_worlds(app::Engine& engine);
    void list_projects(app::Engine& engine);
    static std::string_view open_project_name(const app::Engine& engine);
    // editor.open_world [NAME], editor.open_project [NAME]: as the File menu's two; with no name, they list.
    static void open_world_command(void* user, const console::Args& args, console::Output& out);
    static void open_project_command(void* user, const console::Args& args, console::Output& out);
    // The layout, the Resources' and the editor's settings, written now (unless it's a picture's run).
    void save_user_settings(app::Engine& engine);
    // editor.save, in the console: the same.
    static void save_command(void* user, const console::Args& args, console::Output& out);
    // editor.move NAME X Y Z: that entity to there (world meters), as a command, selected.
    static void move_command(void* user, const console::Args& args, console::Output& out);
    // editor.place PATH X Y Z, editor.duplicate NAME, editor.delete NAME: as dropping, Ctrl+D and Delete do.
    static void place_command(void* user, const console::Args& args, console::Output& out);
    // editor.drop PATH: a model dropped at the middle of the Playground, as a mouse drop there.
    static void drop_command(void* user, const console::Args& args, console::Output& out);
    static void duplicate_command(void* user, const console::Args& args, console::Output& out);
    static void delete_command(void* user, const console::Args& args, console::Output& out);
    // editor.ground NAME: as End does with it selected.
    static void ground_command(void* user, const console::Args& args, console::Output& out);
    // editor.undo, editor.redo: as Ctrl+Z and Ctrl+Y.
    static void undo_command(void* user, const console::Args& args, console::Output& out);
    // editor.select NAME...: those chosen (none: nothing).
    static void select_command(void* user, const console::Args& args, console::Output& out);
    // editor.move_file FROM TO, editor.new_folder PATH, editor.delete_file PATH...: Resources' file operations
    // (file_ops.h), for agents and scripts.
    static void move_file_command(void* user, const console::Args& args, console::Output& out);
    static void new_folder_command(void* user, const console::Args& args, console::Output& out);
    static void delete_file_command(void* user, const console::Args& args, console::Output& out);
    // After files moved or went: Resources and the cells' sources read again.
    void files_changed(app::Engine& engine);
    // editor.frame: as F.
    static void frame_command(void* user, const console::Args& args, console::Output& out);
    // editor.folder PATH [NAME...]: those (or the selection) into a folder.
    static void folder_command(void* user, const console::Args& args, console::Output& out);
    // Each frame: the members of cells spawned since get the folders their files give them (EditorFolder).
    void read_folders(app::Engine& engine);
    // The names given chosen (false, said, if one isn't there); none given: the selection as it is.
    bool choose_named(const console::Args& args, console::Output& out);
    static void redo_command(void* user, const console::Args& args, console::Output& out);
    bool unsaved() const { return commands_.changes() != saved_changes_; }
    // Each frame: the cooker's end, and the members of a cell spawned again (hot reload): the commands and the
    // selection follow them, by their place in the cell's source.
    void follow_saves(app::Engine& engine);

    app::Viewer viewer_;
    CommandStack commands_;
    Selection selection_;
    HierarchyPanel hierarchy_;
    CellMapPanel cell_map_;
    IconSet icons_;
    Gizmo gizmo_;
    Tool tool_ = Tool::Move;
    Space space_ = Space::World;
    HandlesAt handles_at_ = HandlesAt::Pivot;
    Snap snap_;
    Navigation navigation_;
    bool left_press_ = false; // the left button went down on the scene, to select: its release picks
    // Clicks sent to the GPU and not yet answered, the answers seen, and frames waited for the next (a frame that
    // isn't drawn drops a pick).
    uint32_t picks_waiting_ = 0;
    uint32_t pick_answers_ = 0;
    uint32_t pick_wait_frames_ = 0;
    bool pick_flag_ = false; // --pick, until it's answered
    // What a click in the scene does with what it hits: chooses only it, toggles it (Ctrl), adds it (Shift).
    enum class PickMode : uint8_t { Replace, Toggle, Add };
    PickMode pick_mode_ = PickMode::Replace;
    uint32_t pick_flag_x_ = 0;
    uint32_t pick_flag_y_ = 0;
    // The selection's meshes as last gathered: again when it changes, and a few times a second (things get attached).
    core::Array<render::SelectedMesh> selected_meshes_;
    core::Array<flecs::entity_t> selected_entities_; // those with the meshes
    uint64_t gathered_for_ = 0;                      // which were chosen when gathered
    float gathered_s_ = 0.0f;
    PropertiesPanel properties_;
    // What the chosen entity is made from (its prefab, model, effect: src:/ paths), for its properties: found from
    // its entry when another is chosen.
    void update_sources(app::Engine& engine);
    flecs::entity_t sources_of_ = 0;
    // Shows a source file in Resources: its folder opened in the drawer, the file lit for a moment.
    void show_in_resources(std::string_view path);
    Thumbnails thumbnails_;      // before the browser, which draws them
    PreviewPictures pictures_;   // draws the ones the cooker can't: effects, prefabs
    EffectViewer effect_viewer_; // one effect by itself, under a clock of its own (docs/design.md, VFX viewer)
    // Shows an effect's source in the viewer; false, with the reason in message_, if it can't be.
    bool view_effect(app::Engine& engine, std::string_view path);
    static void view_effect_command(void* user, const console::Args& args, console::Output& out);
    static void effect_command(void* user, const console::Args& args, console::Output& out);
    PrefabEditor prefab_editor_; // one prefab's source, to make and change (docs/design.md, The prefab editor)
    // Opens a prefab's source in the prefab editor, or a new one that isn't a file yet; false, with the reason in
    // message_, if it can't be.
    bool edit_prefab(app::Engine& engine, std::string_view path);
    bool new_prefab(app::Engine& engine, std::string_view path);
    // editor.edit_prefab PATH, editor.new_prefab PATH, and editor.prefab ...: what its window does.
    static void edit_prefab_command(void* user, const console::Args& args, console::Output& out);
    static void new_prefab_command(void* user, const console::Args& args, console::Output& out);
    static void prefab_command(void* user, const console::Args& args, console::Output& out);
    // Edit > Make Prefab from Selection: a new prefab (not a file until it's saved) whose parts are the chosen
    // things, placed from the one the handles are on; it opens in the prefab editor. The things themselves stay as
    // they are. False, with the reason in message_, if nothing chosen can be a part.
    bool prefab_from_selection(app::Engine& engine, std::string_view path);
    static void prefab_from_selection_command(void* user, const console::Args& args, console::Output& out);
    // Its question: the prefab's name, in the folder of what its first part is made from.
    void ask_make_prefab(app::Engine& engine);
    void make_prefab_prompt(app::Engine& engine);
    core::FixedString<255> make_prefab_folder_;
    char make_prefab_text_[96] = {};
    bool open_make_prefab_ = false;
    // Edit > Add Light: a point light a little above where the middle of the Playground meets the ground (or at a
    // place given), as a command, chosen. Its values are its properties'; a save writes it into its cell.
    bool add_light(app::Engine& engine, const DVec3* at = nullptr);
    static void add_light_command(void* user, const console::Args& args, console::Output& out);
    // Over the Playground: how far each chosen light reaches (a chosen placement's lights too), as rings about it.
    void draw_light_reach(app::Engine& engine, const View& view, ImDrawList* draw);
    // What an entity's entry says it's made from (src:/ paths; with a prefab, which variant; with a model, whether
    // it's solid). False if it has no entry to read.
    bool read_sources(flecs::entity entity, EntitySources& sources, core::FixedString<31>& variant, bool& solid);
    static PictureEngine picture_engine(app::Engine& engine);
    AssetBrowser browser_;
    bool show_cells_ = false;  // View > Cell Grid
    bool snap_ground_ = false; // the toolbar's surface snapping
    core::Array<flecs::entity_t> collider_entities_{core::heap_allocator(core::MemoryTag::Editor)}; // shown
    float ui_scale_ = 1.0f;             // the user's, on top of the display's
    float applied_ui_scale_ = 0.0f;     // what the style was last made with
    float applied_display_ = 0.0f;      //
    char message_[160] = {};            // the status bar's, when no panel has one
    const char* select_name_ = nullptr; // --select, until it's found
    const char* show_panel_ = nullptr;  // --show, until it's done
    bool playground_hovered_ = false;   // in the last frame's UI
    flecs::entity_t preview_ = 0;       // the model being dragged in, shown where it would go (not a command yet)
    core::FixedString<255> preview_path_;
    const char* preview_flag_ = nullptr; // --preview PATH: as that model held over the Playground's middle (pictures)
    bool looking_ = false;               // a mouse look that started over the Playground
    bool reset_layout_ = false;
    // The drawers, as Unreal's: Resources (Ctrl+Space) and the Output log, up from the bottom bar
    // over the panels while they're wanted, away again at a click elsewhere.
    enum class Drawer : uint8_t { None, Resources, Output };
    Drawer drawer_ = Drawer::None;
    float drawer_share_ = 0.4f; // of the panels' height
    bool status_hovered_ = false;
    char output_filter_[64] = {};
    bool keep_settings_ = true; // the layout and settings are saved at the end: not after a picture
    // Saving.
    CellSources cell_sources_;
    platform::Process cooker_;
    Pending pending_ = Pending::None;
    core::Array<platform::PathString> cooking_;            // what the last save wrote, which the cooker's cooking
    uint64_t saved_changes_ = 0;                           // commands_.changes() at the last save
    app::Engine* engine_ = nullptr;                        // for the console's commands and the maker
    View last_view_;                                       // the Playground's, as last drawn
    std::optional<world::TerrainHeights> terrain_heights_; // the ground's heights, for placing on it (from startup)
    core::HashMap<uint64_t, asset::AssetHandle> holds_;    // by asset ID
    core::Array<Bookmark> bookmarks_{core::heap_allocator(core::MemoryTag::Editor)}; // every world's
    core::HashMap<flecs::entity_t, uint32_t> folders_read_{core::heap_allocator(core::MemoryTag::Editor)};
    MemberTracker members_;               // what the commands and the selection name, through respawns
    uint64_t keyed_changes_ = UINT64_MAX; // commands_.changes() when their keys were noted
    core::Array<flecs::entity_t> scratch_entities_;
    bool quit_ = false;
    // Cooking what changed outside the editor.
    ProjectWatch watch_;
    bool watching_ = false;         // someone's at it: not a picture's run or a counted one
    bool cook_auto_ = true;         // the status bar's switch: cook when the project's files change
    uint32_t fps_cap_kept_ = 0;     // Preferences' frame rate limit: what's saved, whatever --fps-cap says for this run
    bool cook_whole_ = false;       // the cook running is the whole project's (asked for, or a change's), not a save's
    bool cook_failed_ = false;      // the last one left errors
    bool cook_marks_due_ = false;   // a cook has ended: the next look says what the files were left as
    uint32_t cook_marks_look_ = 0;  // that look: the first to start after the cook ended
    uint64_t cook_start_stamp_ = 0; // the watch's stamp when a cook of the whole project started; 0: not known
    uint64_t cooked_stamp_ = 0;     // the stamp of what's cooked; 0: not known
    char cook_state_[96] = {};      // the status bar's: what the last cook came to
    // File > Open World's and Open Project's, listed as each opens.
    core::Array<platform::PathString> world_files_{core::heap_allocator(core::MemoryTag::Editor)};
    core::Array<asset::AssetId> world_ids_{core::heap_allocator(core::MemoryTag::Editor)}; // none: no meta yet
    bool worlds_listed_ = false;
    core::Array<core::FixedString<63>> project_names_{core::heap_allocator(core::MemoryTag::Editor)};
    bool projects_listed_ = false;
    core::FixedString<63> project_to_open_; // while its unsaved prompt is up
};

} // namespace de::editor
#endif
