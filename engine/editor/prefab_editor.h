#pragma once

#include "app/preview_scene.h"
#include "asset/registry.h"
#include "core/allocator.h"
#include "core/array.h"
#include "core/fixed_string.h"
#include "core/math.h"
#include "editor/commands.h"
#include "editor/gizmo.h"
#include "editor/orbit_camera.h"
#include "editor/prefab_document.h"
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

// The prefab editor (docs/design.md, The prefab editor): one .prefab source, open by itself, to make and
// change. A window of the editor's, beside the Playground by default: the prefab in a preview scene, drawn by the
// engine's renderer, with a camera that turns about it; its parts listed beside it, and the chosen part's own
// values under them (its ID, what it's made from, whether it's solid, where it is); its variants, each shown by
// itself to tick what it keeps.
//
// What it edits is the document (prefab_document.h), not the cooked prefab: the scene is made from the document as
// it stands, each part an entity with the cooked model, effect or prefab its file cooks to, so a change shows the
// frame it's made, before any save or cook. Parts come in from Resources (dragged onto the picture or the list);
// the handles move, turn and scale the chosen one, as the Playground's do; a click in the picture chooses what's
// under it. Every change is one step of its own undo (Ctrl+Z in its window), which keeps the document's text at each
// step. Save (Ctrl+S in its window) writes the file; the editor then cooks, and what's placed in the world follows
// by hot reload. Closing, or opening another, with changes unsaved asks first.
class PrefabEditor {
public:
    static constexpr const char* kWindow = "Prefab";

    explicit PrefabEditor(core::Allocator& allocator);
    ~PrefabEditor() = default;
    PrefabEditor(const PrefabEditor&) = delete;
    PrefabEditor& operator=(const PrefabEditor&) = delete;
    PrefabEditor(PrefabEditor&&) = delete;
    PrefabEditor& operator=(PrefabEditor&&) = delete;

    // Opens a prefab's source ("src:/content/props/candle.prefab") in place of what it showed, and brings its window
    // forward. With changes unsaved in the one it shows, it asks first and opens once that's answered (true). False,
    // saying why in `why`, if it isn't a prefab the editor can read.
    bool open(const PictureEngine& engine, std::string_view path, std::span<char> why);
    // The same for a prefab that isn't a file yet: nothing in it, written by its first save.
    bool open_new(const PictureEngine& engine, std::string_view path, std::span<char> why);
    // The same, starting with these parts (their IDs made fit and their own: a part's is what it's wanted to be
    // called). Refused while the prefab it shows has changes unsaved: there's no asking with parts in hand.
    bool open_new_with(const PictureEngine& engine, std::string_view path, std::span<const PrefabDocPart> parts,
                       std::span<char> why);
    // Lets go of it, saved or not.
    void close(const PictureEngine& engine);
    bool is_open() const { return scene_.has_value(); }
    std::string_view path() const { return path_.view(); }
    // It has changes its file doesn't.
    bool dirty() const { return is_open() && step_ != saved_step_; }
    const PrefabDocument& document() const { return doc_; }

    // The edits its window makes, for the console and tests too. Each is one step of undo. They say why not in
    // message().
    // A part from a source file (a model, an effect or another prefab: a src:/ path), at a place from the prefab's
    // origin; chosen. Its ID is from the file's name.
    bool add_part(std::string_view source, const Vec3& position);
    bool remove_part(std::string_view id);
    bool duplicate_part(std::string_view id);
    bool rename_part(std::string_view id, std::string_view to);
    bool place_part(std::string_view id, const Vec3& position, const Vec3& rotation_deg, const Vec3& scale);
    // Gives a part a light (as its Light rows do), or with null takes it off.
    bool set_light(std::string_view id, const asset::LightDesc* light);
    // Chooses a part (empty: none). False if there's none of that ID.
    bool select(std::string_view id);
    std::string_view selected() const;
    bool add_variant(std::string_view name);
    bool remove_variant(std::string_view name);
    // Whether a variant leaves a part out.
    bool leave_out(std::string_view variant, std::string_view part, bool out);
    // Which variant the picture shows (empty: the prefab as it is). False if it has none of that name.
    bool show_variant(std::string_view name);
    bool undo();
    bool redo();
    bool can_undo() const { return step_ > 0; }
    bool can_redo() const { return step_ + 1 < steps_.size(); }
    // Writes its file. False, with message() saying why, if it can't be (nothing in it yet, or the write failed).
    bool save(const PictureEngine& engine);
    // What the last thing it did came to, for the status line and the console.
    const char* message() const { return message_; }
    // What's wrong with it as it stands (what the cooker would refuse, and files it names that aren't there).
    std::span<const core::FixedString<159>> problems() const { return problems_.span(); }

    // What the editor does for it, each asked once: cook (a save wrote its file); show a file in Resources.
    bool take_saved();
    bool take_show_file(core::FixedString<255>& path);
    // The window's close button with changes unsaved: asked first. True if there's nothing to ask; false while it
    // asks, and take_quit() is true once the answer lets the editor close.
    bool may_quit();
    bool take_quit();

    // Each frame it's open, before the frame draws: the scene made to match the document, the camera, the light.
    void update(const PictureEngine& engine, float frame_s);
    // From Application::draw: its scene's passes, while its picture shows.
    void draw();
    // Its window. dock_beside: the window it joins as a tab the first time it's shown. resources_file: the file
    // chosen in Resources now (empty: none), which Add takes.
    void ui(const PictureEngine& engine, const char* dock_beside, std::string_view resources_file);
    // Its picture is on screen this frame, this many pixels: what its window says, and a test in its place.
    void show(uint32_t width, uint32_t height);
    app::PreviewScene* scene() { return scene_ ? &*scene_ : nullptr; }
    // Looks at the whole prefab again, or at the chosen part.
    void frame(bool chosen_only);
    void shutdown(const PictureEngine& engine) { close(engine); }

private:
    enum class Pending : uint8_t { None, Close, Open, OpenNew, Quit };
    enum class Backdrop : uint8_t { Dark, Grey, Light, Count };
    // What a part is in the scene.
    struct PartView {
        flecs::entity_t entity = 0;
        bool mesh_missing = false; // it names one that isn't there, or isn't cooked
        bool effect_missing = false;
        bool prefab_missing = false;
    };

    bool load(const PictureEngine& engine, std::string_view path, bool is_new, std::span<char> why);
    bool ensure_scene(const PictureEngine& engine);
    // The document changed: a step of undo, the scene and the problems made to match.
    void commit();
    void restore(size_t step);
    void say(const char* format, std::string_view about = "", std::string_view more = "");
    // A source a part names, as the asset it cooks to: loaded and held. False if it isn't there or isn't cooked.
    bool resolve(const PictureEngine& engine, std::string_view relative, std::string_view kind, asset::AssetType type,
                 asset::AssetHandle& out);
    void rebuild(const PictureEngine& engine);
    void clear_scene(const PictureEngine& engine);
    void place_parts();
    void hide_left_out(const PictureEngine& engine);
    void measure(const PictureEngine& engine);
    void refresh_problems();
    uint64_t structure_stamp() const;
    uint32_t selected_index() const;
    bool part_shown(uint32_t part) const;
    // The part an instance slot of the scene belongs to, or UINT32_MAX.
    uint32_t part_of_slot(uint32_t slot);
#if DE_BUILD_DEV
    void gather_outline();
    void take_pick();
#endif
    void finish_pending(const PictureEngine& engine);

    // Its toolbar, in as many lines as it needs at that width: how high it came to.
    float view_bar(const PictureEngine& engine, float width);
    void picture(ImVec2 size);
    void side(const PictureEngine& engine, float width, std::string_view resources_file);
    void parts_list(const PictureEngine& engine, std::string_view resources_file);
    void part_properties(const PictureEngine& engine);
    void inner_parts(const PictureEngine& engine, uint32_t part, uint32_t variant);
    void variants_list();
    void unsaved_prompt(const PictureEngine& engine);
    // Adds to a subject what one entity of the scene takes up: its mesh's box, its effect's reach.
    void add_bounds(const PictureEngine& engine, flecs::entity entity, PictureSubject& subject) const;
    void part_bounds(const PictureEngine& engine, uint32_t part, PictureSubject& subject);
    uint32_t shown_variant_index() const;

    core::Allocator* allocator_;
    std::optional<app::PreviewScene> scene_;
    core::FixedString<255> path_;   // its source, a src:/ path
    core::FixedString<255> folder_; // the folder it's in: what its parts' paths are relative to
    bool is_new_ = false;           // not a file yet
    PrefabDocument doc_;
    core::Array<char> text_;               // scratch: the document's text
    core::Array<core::Array<char>> steps_; // undo: the document's text at each step
    size_t step_ = 0;                      // the one it's at
    size_t saved_step_ = SIZE_MAX;         // the one its file holds; SIZE_MAX: none (new, or its step was dropped)
    core::FixedString<31> selected_;       // a part's ID; empty: none
    core::FixedString<31> shown_variant_;  // empty: the prefab as it is
    core::Array<core::FixedString<159>> problems_;
    char message_[200] = {};
    float message_left_s_ = 0.0f; // how much longer the picture shows it

    // The scene.
    core::Array<PartView> views_;          // by part
    core::Array<asset::AssetHandle> held_; // what the parts draw with
    uint64_t built_stamp_ = 0;             // structure_stamp() of what the scene was made from
    bool rebuild_ = true;
    PictureSubject bounds_;        // all of it
    PictureSubject chosen_bounds_; // the chosen part
    double measure_in_s_ = 0.0;
    bool auto_frame_ = true; // it keeps the whole prefab in view, until the camera is taken hold of
    size_t framed_of_ = 0;   // how much there was of it when it last did
    OrbitCamera camera_{};
    PictureFraming framing_{};
    render::SceneLighting lighting_{};
    Backdrop backdrop_ = Backdrop::Grey;
    bool grid_ = true;
    bool studio_ = true; // the studio's light is on; off, only the prefab's own lights show
    core::Array<world::DebugLine> lines_;

    // The handles, and what's chosen in the picture.
    Gizmo gizmo_;
    CommandStack commands_; // what the handles write through: the document takes what they did, and keeps the undo
    Tool tool_ = Tool::Move;
    Space space_ = Space::World;
    Snap snap_;
    bool was_dragging_ = false;
    bool left_press_ = false;
#if DE_BUILD_DEV
    core::Array<render::SelectedMesh> outline_{core::heap_allocator(core::MemoryTag::Editor)};
    render::EditorOverlay overlay_{};
    uint32_t pick_answers_ = 0;
    uint32_t picks_waiting_ = 0;
    uint32_t pick_wait_frames_ = 0;
#endif

    // Its window.
    bool window_open_ = false;
    bool focus_ = false;
    bool visible_ = false;
    Pending pending_ = Pending::None;
    core::FixedString<255> pending_path_;
    bool quit_ = false;
    bool saved_ = false; // a save wrote its file: the editor cooks
    core::FixedString<255> show_file_;
    char id_text_[32] = {};       // the ID field, while it's typed in
    core::FixedString<31> id_of_; // whose it holds
    bool rename_focus_ = false;   // the ID field takes the keyboard next frame
    char variant_text_[32] = {};  // a new variant's name
    bool field_edited_ = false;   // a value field changed the document: committed when it's let go
};

} // namespace de::editor
