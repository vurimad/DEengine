#pragma once

#include "core/allocator.h"
#include "core/array.h"
#include "core/fixed_string.h"
#include "core/hash_map.h"
#include "editor/commands.h"
#include "editor/outline.h"
#include "editor/selection.h"

#include <flecs.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace de::app {
struct Engine;
}

namespace de::editor {

// The hierarchy: what's placed in the world, by name, each with what's attached to it under it, in the designer's
// folders (EditorFolder: saved in the cells' files, only for organising), folders first. Cells don't show here: they're
// how the world is stored and streamed (the cell map shows them). A click on a folder chooses everything in it; a
// double-click renames it; rows dragged onto a folder go into it, onto the space below the rows out of every folder; a
// folder dragged onto another goes inside it. The right button: Move to New Folder, Move Out of Folders, and on a
// folder, Rename and Remove Folder (what's in it moves up a level). Each change is a command. Drawn as the user's tree:
// a search field, rows on the navigation surface selected whole, a disclosure triangle for what has children; what each
// is shows on hover; the number of rows below. A click selects; a double-click opens or closes a branch, or frames the
// camera on a leaf (F frames the selection). Up and down move the selection, right opens or goes in, left closes or
// goes out. A selection made elsewhere opens the tree down to it. It reads the world four times a second, and when a
// branch opens or closes.
class HierarchyPanel {
public:
    static constexpr const char* kWindow = "Hierarchy";

    explicit HierarchyPanel(core::Allocator& allocator)
        : outline_(allocator), rows_(allocator), roots_(allocator), open_(allocator) {}

    // hidden: an entity left out (the editor's own camera).
    void draw(const app::Engine& engine, Selection& selection, flecs::entity_t hidden, CommandStack& commands);

    // The folder edits, as commands (one undo each): the entities into a folder ("" out of every one); a folder's
    // path changed for everything in it and under it; a new folder (named apart, beside the primary's) with the
    // chosen in it, its name then edited in place.
    static void move_to_folder(const flecs::world& ecs, CommandStack& commands,
                               std::span<const flecs::entity_t> entities, std::string_view path);
    static void rename_folder(const flecs::world& ecs, CommandStack& commands, std::string_view from,
                              std::string_view to);
    // The world changed under it (another world opened): read it again now, every branch closed.
    void reset();
    // What the status bar shows while the panel has focus.
    const char* status() const { return status_; }
    bool focused() const { return focused_; }

private:
    using FolderPath = core::FixedString<127>;
    struct Row {
        flecs::entity_t entity = 0; // 0: a folder's row
        uint32_t depth = 0;
        bool has_children = false;
        int32_t folder = -1; // into folders_, for a folder's row
    };
    struct Root {
        flecs::entity_t entity = 0;
        core::FixedString<63> name;
        FolderPath folder;
    };

    void build_rows(const app::Engine& engine, flecs::entity_t hidden);
    void add_items(std::span<const Outline::Item> items, uint32_t depth);
    bool is_open(const Row& row) const;
    void add_folder(std::string_view path, uint32_t depth);
    // The chosen that have a place in the hierarchy (not the hidden one), for moving into folders.
    void chosen_rows(const app::Engine& engine, const Selection& selection, core::Array<flecs::entity_t>& out) const;
    void choose_folder(const Selection& before, Selection& selection, std::string_view path, bool add) const;
    void start_rename(std::string_view path);
    void new_folder(const app::Engine& engine, const Selection& selection, CommandStack& commands);
    void context_menu(const app::Engine& engine, Selection& selection, CommandStack& commands);
    void set_open(const Row& row, bool open);
    void keyboard(Selection& selection);
    // Shift-click: the rows from one to the other chosen, as they're listed.
    void choose_range(Selection& selection, flecs::entity_t from, flecs::entity_t to) const;
    // Opens the branches down to an entity chosen elsewhere (a link, --select), and scrolls to it.
    void reveal(const flecs::world& ecs, flecs::entity_t entity);

    Outline outline_;
    core::Array<Row> rows_;
    core::Array<Root> roots_;                      // what's attached to nothing, by name
    core::HashMap<flecs::entity_t, uint8_t> open_; // branches open (the value is unused)
    core::Array<FolderPath> folders_{core::heap_allocator(core::MemoryTag::Editor)}; // every folder, parents first
    core::HashMap<uint64_t, uint8_t> closed_{core::heap_allocator(core::MemoryTag::Editor)}; // folders closed, by path
    uint64_t chosen_folder_ = 0; // the folder last clicked (its path's hash), lit while what it chose is chosen
    FolderPath renaming_;        // the folder whose name is being edited in place ("": none)
    char rename_text_[128] = {};
    bool rename_focus_ = false;
    int32_t menu_folder_ = -1; // the folder the right button was pressed on (into folders_)
    flecs::entity_t hidden_ = 0;
    char filter_[96] = {};
    uint64_t next_read_ns_ = 0;
    bool rows_stale_ = true;
    char status_[64] = {};
    bool focused_ = false;
    flecs::entity_t revealed_ = 0;  // the selection last shown
    flecs::entity_t scroll_to_ = 0; // a row to bring into view
};

} // namespace de::editor
