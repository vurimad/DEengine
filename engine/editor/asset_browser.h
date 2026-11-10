#pragma once

#include "core/allocator.h"
#include "core/array.h"
#include "core/fixed_string.h"
#include "core/hash_map.h"
#include "editor/file_ops.h"
#include "editor/selection.h"
#include "editor/thumbnails.h"

#include <imgui.h>

#include <cstdint>
#include <string_view>

namespace de::platform {
class Vfs;
}

namespace de::editor {

// The resources: the source tree's content/ (through src:/), in the user's resource browser design. A toolbar (back,
// forward, refresh; where you are, a step per folder; favourite folders; view options), then the folder tree on the
// navigation surface beside the folder's contents, as a list or a grid of cards (Ctrl+wheel sizes them), with a search
// over every file and folder above them. One-column layout: a single tree with the files in it. A click selects (Ctrl
// adds, Shift takes a range); a double-click or Enter opens a folder, or asks the editor to open a file (a world).
// Items drag out (payload kSourceFilePayload: the path, terminated). Read once, and again on Refresh. Kept between
// sessions in user:/editor/resources.settings: the view, the layout, the card size, the favourites, the folder shown,
// the folders open in the tree, what's selected, the search and the folder tree's width.
class AssetBrowser {
public:
    static constexpr const char* kWindow = "Resources";
    static constexpr const char* kSourceFilePayload = "DE_SOURCE_FILE";

    explicit AssetBrowser(core::Allocator& allocator);

    // True when a file was opened (double-click or Enter): the selection holds it.
    // Into the window it's drawn in (the editor's Resources drawer).
    bool draw(const platform::Vfs& vfs, Selection& selection);
    // What the status bar shows while the panel has focus: "12 items  1 selected".
    const char* status() const { return status_; }
    bool focused() const { return focused_; }

    // Opens the folder a file is in, the tree open down to it ("src:/content/props/rock.gltf"), once it's read.
    void show_file(std::string_view path) { show_file_ = Path(path); }
    // Read the folders again next time it's drawn (files changed under it: a console command, a cook).
    void refresh() { refresh_ = true; }
    // Whether files were moved, renamed or deleted since last asked (the editor reads its cells' sources again).
    bool take_files_changed() {
        const bool changed = files_changed_;
        files_changed_ = false;
        return changed;
    }
    // Models' pictures on their cards and rows; null: their glyphs.
    void set_thumbnails(Thumbnails* thumbnails) { thumbnails_ = thumbnails; }

    void load_settings(const platform::Vfs& vfs);
    void save_settings(const platform::Vfs& vfs) const;

private:
    enum class ViewMode : uint8_t { List, Grid };
    enum class Layout : uint8_t { TwoColumns, OneColumn };
    enum class NavIcon : uint8_t { Back, Forward, Refresh };
    enum class SelectMode : uint8_t { Replace, Toggle, Range, AddRange };
    using Path = core::FixedString<255>;

    struct Entry {
        uint32_t name = 0; // into names_
        uint32_t name_length = 0;
        uint32_t parent = 0;
        uint32_t first_child = 0; // entries_[first_child, first_child + child_count): its own, folders first
        uint32_t child_count = 0;
        uint64_t key = 0; // its path's hash: what the tree's open folders are kept by
        bool directory = false;
    };

    void scan(const platform::Vfs& vfs);
    std::string_view name_of(uint32_t entry) const;
    // "src:/content/terrain/wc.world"; false if it doesn't fit.
    bool path_of(uint32_t entry, Path& out) const;
    uint32_t find(std::string_view path) const;
    bool has_child_folders(uint32_t folder) const;

    void navigate(uint32_t folder, bool remember = true);
    void reveal(uint32_t entry);
    void open_item(uint32_t entry, Selection& selection);
    void select(uint32_t entry, SelectMode mode, Selection& selection);
    bool is_selected(uint32_t entry) const;
    static SelectMode select_mode();

    void draw_toolbar();
    bool nav_button(const char* id, NavIcon icon, const char* tip);
    void draw_breadcrumbs();
    void draw_favorites();
    void draw_view_options();
    void draw_tree(uint32_t folder, bool include_files, Selection& selection);
    void draw_contents(Selection& selection, float region_min_x, float region_max_x);
    void draw_content_controls();
    bool view_mode_button(const char* id, ViewMode mode, const char* tip);
    void draw_list(Selection& selection);
    void draw_grid(Selection& selection);
    void item_interaction(uint32_t entry, bool clicked, bool double_clicked, Selection& selection);
    void item_drag_source(uint32_t entry);
    void item_context_menu(uint32_t entry, Selection& selection);
    void keyboard(Selection& selection, uint32_t columns);
    void rebuild_shown();
    // File operations (file_ops.h): rename (F2), delete to the recycle bin (Delete, after saying who still names
    // it), a new folder, and moving by dropping onto a folder.
    void folder_drop_target(uint32_t folder, Selection& selection);
    void apply_moves(std::span<const SourceMove> moves, Selection& selection);
    void start_rename(uint32_t entry);
    void ask_delete(uint32_t entry);
    void new_folder(uint32_t parent);
    void draw_file_dialogs(Selection& selection);
    // A model's picture, if it's made (and asked for if not).
    bool model_picture(uint32_t entry, Thumbnails::Picture& out);

    core::Array<char> names_;
    core::Array<Entry> entries_;     // [0]: content/ itself; each folder's entries together, folder by folder
    core::Array<uint32_t> shown_;    // the contents' entries this frame: the folder's, or a search's matches
    core::Array<uint32_t> selected_; // selected entries
    core::Array<uint32_t> back_;
    core::Array<uint32_t> forward_;
    core::Array<Path> favorites_;
    core::HashMap<uint64_t, uint8_t> tree_open_; // folders open in the tree, by path hash (the value is unused)
    uint32_t current_ = 0;                       // the folder shown
    uint32_t anchor_ = UINT32_MAX;               // where a Shift range starts
    uint32_t opened_ = UINT32_MAX;               // a file opened this frame
    uint32_t context_entry_ = UINT32_MAX;        // the item the context menu is for
    ImGuiID item_menu_ = 0;                      // its popup, opened from rows nested anywhere
    ViewMode mode_ = ViewMode::Grid;
    Layout layout_ = Layout::TwoColumns;
    float grid_extent_ = 96.0f; // card thumbnails, screen pixels at 100%
    char search_[96] = {};
    char status_[96] = {};
    Thumbnails* thumbnails_ = nullptr;
    const platform::Vfs* vfs_ = nullptr; // while drawn
    bool files_changed_ = false;
    // The rename dialog: what's renamed, its new name; the delete dialog: what goes, who still names it.
    Path renaming_;
    char rename_text_[128] = {};
    bool open_rename_ = false;
    Path rename_after_scan_; // a folder just made: renamed once the folders are read again
    core::Array<SourcePath> deleting_;
    core::Array<SourcePath> users_;
    bool open_delete_ = false;
    Path show_file_; // to open when next drawn
    // Kept from the last session, applied once the folders are read: the folder shown and what was selected there.
    Path start_folder_;
    core::Array<Path> start_selected_;
    float folders_width_ = 260.0f; // the tree's column, screen pixels at 100%
    bool scanned_ = false;
    bool refresh_ = false; // asked from the toolbar: read the folders again
    bool scroll_to_top_ = false;
    bool focused_ = false;
};

} // namespace de::editor
