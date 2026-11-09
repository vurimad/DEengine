#pragma once

#include "app/framework.h"
#include "app/viewer.h"
#include "editor/asset_browser.h"
#include "editor/cell_map_panel.h"
#include "editor/commands.h"
#include "editor/gizmo.h"
#include "editor/hierarchy_panel.h"
#include "editor/icons.h"
#include "editor/navigation.h"
#include "editor/properties_panel.h"
#include "editor/selection.h"

#if DE_BUILD_DEV
namespace de::editor {

// deengine --editor, in dev builds (docs/design.md, Editor & tooling): the viewer's world with the editor's panels
// around it, in the user's editor design (theme.h): a header row (the name, the menus), the panels docked with thin
// gaps between them, and a status bar (what the focused panel reports; the camera's speed). The hierarchy, with the
// cell map in a tab beside it, sits over the properties on the left; the game's view (the Playground) on the right over
// the resources. The frame stats (F1, or View) start hidden. The camera moves as in Unity's scene view: the right
// button held over the Playground flies it (WASD, the mouse to look, the wheel its speed), the wheel zooms, a middle
// drag pans, Alt and a left drag orbits, Alt and a right drag zooms, F frames the selection (navigation.h). The
// Playground's toolbar: undo and redo, the tools (select, move, rotate, scale: Q W E R), world or local axes, snapping
// to a grid, to angles and to scale steps, the camera's speed, the view, framing the selection. A click in the scene
// selects what's under the mouse; the selection's handles move, turn and scale it. Ctrl+Z undoes, Ctrl+Y or
// Ctrl+Shift+Z redoes, F frames the selected entity, a double-click on a world in the resources opens it. Escape
// doesn't quit; closing the window does. The layout is kept in user:/editor/layout.ini, the interface scale, the tool
// and the snapping in user:/editor/editor.settings. Flags: the viewer's (app/viewer.h), and, for pictures of the editor
// taken from the command line (the agent's eyes):
//   --select NAME   select the entity of that name once it's in the world
//   --select-file PATH   select that source file ("src:/content/terrain/wc.world")
//   --show PANEL    bring that panel's tab to the front ("Cell Map")
//   --pick X,Y      click that pixel of the Playground's scene, once it's drawn: select what's there
//   --tool NAME     select, move, rotate or scale
//   --handles AT    pivot or center
//   --drag H,AMOUNT show a drag under way on handle H (0-2 an axis, 3-5 a plane, 6 the middle), gone AMOUNT (meters,
//                   or radians turning), changing nothing
// A scripted run (a picture) keeps nothing: not the layout, not the settings.
class Editor final : public app::Application {
public:
    static constexpr const char* kPlayground = "Playground";

    Editor();
    bool startup(app::Engine& engine, const app::FrameworkArgs& args) override;
    bool update(app::Engine& engine, float frame_s) override;
    void ui(app::Engine& engine) override;
    void shutdown(app::Engine& engine) override;

private:
    void header_row(app::Engine& engine);
    void dock_space(ImVec2 size);
    void status_bar(ImVec2 size);
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
    void open_world(app::Engine& engine);
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
    uint32_t pick_flag_x_ = 0;
    uint32_t pick_flag_y_ = 0;
    // The selection's meshes as last gathered: again when it changes, and a few times a second (things get attached).
    core::Array<render::SelectedMesh> selected_meshes_;
    core::Array<flecs::entity_t> selected_entities_; // those with the meshes
    flecs::entity_t gathered_for_ = 0;
    float gathered_s_ = 0.0f;
    PropertiesPanel properties_;
    AssetBrowser browser_;
    float ui_scale_ = 1.0f;             // the user's, on top of the display's
    float applied_ui_scale_ = 0.0f;     // what the style was last made with
    float applied_display_ = 0.0f;      //
    char message_[160] = {};            // the status bar's, when no panel has one
    const char* select_name_ = nullptr; // --select, until it's found
    const char* show_panel_ = nullptr;  // --show, until it's done
    bool playground_hovered_ = false;   // in the last frame's UI
    bool looking_ = false;              // a mouse look that started over the Playground
    bool reset_layout_ = false;
    bool keep_settings_ = true; // the layout and settings are saved at the end: not after a picture
    bool quit_ = false;
};

} // namespace de::editor
#endif
