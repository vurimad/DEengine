#include "editor/hierarchy_panel.h"

#include "app/framework.h"
#include "core/hash.h"
#include "core/profile.h"
#include "editor/entity_record.h"
#include "editor/theme.h"
#include "editor/widgets.h"
#include "platform/time.h"
#include "world/cell.h"
#include "world/transform.h"
#include "world/world.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace de::editor {

namespace {

constexpr uint64_t kReadEveryNs = platform::kNsPerSecond / 4;
constexpr size_t kMaxMatches = 5000; // a search shows at most this many
constexpr const char* kRowsPayload = "DE_HIERARCHY_ROWS";
constexpr const char* kFolderPayload = "DE_HIERARCHY_FOLDER";
constexpr const char* kMenu = "##hierarchy menu";

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool name_before(std::string_view a, std::string_view b) {
    return std::ranges::lexicographical_compare(a, b, [](char x, char y) { return lower(x) < lower(y); });
}

// A folder's path in order: segment by segment, so a folder's children follow it ("a/b" before "a b").
bool folder_before(std::string_view a, std::string_view b) {
    return std::ranges::lexicographical_compare(a, b, [](char x, char y) {
        const char lx = x == '/' ? '\x01' : lower(x);
        const char ly = y == '/' ? '\x01' : lower(y);
        return lx < ly;
    });
}

std::string_view parent_of(std::string_view path) {
    const size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string_view() : path.substr(0, slash);
}

std::string_view leaf_of(std::string_view path) {
    const size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// Whether path is folder or inside it.
bool within(std::string_view path, std::string_view folder) {
    return !folder.empty() && path.starts_with(folder) && (path.size() == folder.size() || path[folder.size()] == '/');
}

uint64_t folder_key(std::string_view path) {
    return core::hash_bytes(path.data(), path.size()) | 1U;
}

// A folder name as a file can hold it: no quotes or backslashes, no slashes at its ends, no empty steps.
void clean_path(std::string_view text, char (&out)[128]) {
    size_t length = 0;
    for (const char c : text) {
        if (c == '"' || c == '\\' || length + 1 >= sizeof(out)) {
            continue;
        }
        if (c == '/' && (length == 0 || out[length - 1] == '/')) {
            continue;
        }
        out[length++] = c;
    }
    while (length > 0 && (out[length - 1] == '/' || out[length - 1] == ' ')) {
        --length;
    }
    out[length] = '\0';
}

const char* folder_of(const flecs::entity entity) {
    const auto* folder = entity.try_get<EditorFolder>();
    return folder != nullptr ? folder->path : "";
}

} // namespace

void HierarchyPanel::reset() {
    open_.clear();
    rows_.clear();
    roots_.clear();
    folders_.clear();
    next_read_ns_ = 0;
    rows_stale_ = true;
    revealed_ = 0;
    chosen_folder_ = 0;
    renaming_ = {};
}

bool HierarchyPanel::is_open(const Row& row) const {
    if (row.folder >= 0) {
        return !closed_.contains(folder_key(folders_[static_cast<size_t>(row.folder)].view()));
    }
    return open_.contains(row.entity);
}

void HierarchyPanel::set_open(const Row& row, bool open) {
    if (!row.has_children || is_open(row) == open) {
        return;
    }
    if (row.folder >= 0) {
        const uint64_t key = folder_key(folders_[static_cast<size_t>(row.folder)].view());
        if (open) {
            static_cast<void>(closed_.remove(key));
        } else {
            closed_.set(key, 1);
        }
    } else if (open) {
        open_.set(row.entity, 1);
    } else {
        static_cast<void>(open_.remove(row.entity));
    }
    rows_stale_ = true;
}

void HierarchyPanel::reveal(const flecs::world& ecs, flecs::entity_t entity) {
    // Every parent up from it opens, and the folders it's in.
    flecs::entity at = ecs.entity(entity);
    for (int guard = 0; guard < 64 && at.is_valid(); ++guard) {
        flecs::entity parent;
        if (const auto* attached = at.try_get<flecs::Parent>(); attached != nullptr && ecs.is_alive(attached->value)) {
            parent = ecs.entity(attached->value);
        } else {
            parent = at.parent();
        }
        if (!parent.is_valid()) {
            break;
        }
        open_.set(parent.id(), 1);
        at = parent;
    }
    for (std::string_view folder = folder_of(at); !folder.empty(); folder = parent_of(folder)) {
        static_cast<void>(closed_.remove(folder_key(folder)));
    }
    scroll_to_ = entity;
    rows_stale_ = true;
}

void HierarchyPanel::add_items(std::span<const Outline::Item> items, uint32_t depth) {
    for (const Outline::Item& item : items) {
        const std::span<const Outline::Item> children = outline_.children(item.entity);
        rows_.push({.entity = item.entity, .depth = depth, .has_children = !children.empty()});
        if (!children.empty() && open_.contains(item.entity)) {
            add_items(children, depth + 1);
        }
    }
}

void HierarchyPanel::add_folder(std::string_view path, uint32_t depth) {
    // Its folders first, then what's in it by name, each with what's attached to it.
    for (size_t f = 0; f < folders_.size(); ++f) {
        if (parent_of(folders_[f].view()) != path) {
            continue;
        }
        rows_.push({.entity = 0, .depth = depth, .has_children = true, .folder = static_cast<int32_t>(f)});
        if (!closed_.contains(folder_key(folders_[f].view()))) {
            add_folder(folders_[f].view(), depth + 1);
        }
    }
    for (const Root& root : roots_) {
        if (root.folder.view() != path) {
            continue;
        }
        const std::span<const Outline::Item> children = outline_.children(root.entity);
        rows_.push({.entity = root.entity, .depth = depth, .has_children = !children.empty()});
        if (!children.empty() && open_.contains(root.entity)) {
            add_items(children, depth + 1);
        }
    }
}

void HierarchyPanel::build_rows(const app::Engine& engine, flecs::entity_t hidden) {
    DE_PROFILE_ZONE("HierarchyPanel::build_rows");
    rows_.clear();
    const flecs::world& ecs = engine.world.ecs();
    char label[128];
    const std::string_view filter(filter_);
    if (!filter.empty()) {
        // A search: every entity whose name holds the text, in one flat list.
        for (const Outline::Item& item : outline_.items()) {
            if (rows_.size() >= kMaxMatches) {
                break;
            }
            if (item.entity == hidden) {
                continue;
            }
            entity_label(engine.assets, ecs.entity(item.entity), label);
            if (contains_ignoring_case(label, filter)) {
                rows_.push({.entity = item.entity, .depth = 0, .has_children = false});
            }
        }
        return;
    }
    // What's attached to nothing, by name, in its folder; the folders, each with those above it.
    roots_.clear();
    folders_.clear();
    for (const Outline::Item& item : outline_.roots()) {
        if (item.entity == hidden || !ecs.is_alive(item.entity)) {
            continue;
        }
        const flecs::entity entity = ecs.entity(item.entity);
        entity_label(engine.assets, entity, label);
        Root& root = roots_.push({.entity = item.entity, .name = {}, .folder = {}});
        static_cast<void>(root.name.assign(label));
        static_cast<void>(root.folder.assign(folder_of(entity)));
        for (std::string_view folder = root.folder.view(); !folder.empty(); folder = parent_of(folder)) {
            if (std::ranges::none_of(folders_, [folder](const FolderPath& had) { return had.view() == folder; })) {
                folders_.push(FolderPath(folder));
            }
        }
    }
    // The one being renamed stays, though nothing may be in it yet (a new one).
    if (!renaming_.empty() &&
        std::ranges::none_of(folders_, [this](const FolderPath& had) { return had.view() == renaming_.view(); })) {
        folders_.push(renaming_);
    }
    std::ranges::sort(roots_, [](const Root& a, const Root& b) { return name_before(a.name.view(), b.name.view()); });
    std::ranges::sort(folders_,
                      [](const FolderPath& a, const FolderPath& b) { return folder_before(a.view(), b.view()); });
    add_folder({}, 0);
}

void HierarchyPanel::choose_folder(const Selection& before, Selection& selection, std::string_view path,
                                   bool add) const {
    // Everything in it and in its folders.
    if (!add) {
        selection.select_entity(0);
    }
    static_cast<void>(before);
    for (const Root& root : roots_) {
        if (within(root.folder.view(), path)) {
            selection.add_entity(root.entity);
        }
    }
}

void HierarchyPanel::keyboard(Selection& selection) {
    if (!ImGui::IsWindowFocused() || ImGui::IsAnyItemActive() || rows_.empty()) {
        return;
    }
    size_t at = rows_.size();
    for (size_t i = 0; i < rows_.size(); ++i) {
        const bool lit = rows_[i].folder >= 0
                             ? chosen_folder_ == folder_key(folders_[static_cast<size_t>(rows_[i].folder)].view())
                             : selection.kind == Selection::Kind::Entity && rows_[i].entity == selection.entity;
        if (lit) {
            at = i;
            break;
        }
    }
    const auto select = [&](size_t index) {
        if (index >= rows_.size()) {
            return;
        }
        if (rows_[index].folder >= 0) {
            const std::string_view path = folders_[static_cast<size_t>(rows_[index].folder)].view();
            choose_folder(selection, selection, path, false);
            chosen_folder_ = folder_key(path);
            revealed_ = selection.entity;
            return;
        }
        chosen_folder_ = 0;
        selection.select_entity(rows_[index].entity);
        revealed_ = rows_[index].entity;
        scroll_to_ = rows_[index].entity;
    };
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        select(at == rows_.size() ? 0 : std::min(at + 1, rows_.size() - 1));
    } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        select(at == rows_.size() || at == 0 ? 0 : at - 1);
    } else if (at < rows_.size() && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        // Open it, or go to its first child.
        if (rows_[at].has_children && !is_open(rows_[at])) {
            set_open(rows_[at], true);
        } else if (rows_[at].has_children && at + 1 < rows_.size()) {
            select(at + 1);
        }
    } else if (at < rows_.size() && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        // Close it, or go to its parent.
        if (rows_[at].has_children && is_open(rows_[at])) {
            set_open(rows_[at], false);
        } else {
            for (size_t i = at; i-- > 0;) {
                if (rows_[i].depth < rows_[at].depth) {
                    select(i);
                    break;
                }
            }
        }
    } else if (at < rows_.size() && rows_[at].folder >= 0 && ImGui::IsKeyPressed(ImGuiKey_F2)) {
        start_rename(folders_[static_cast<size_t>(rows_[at].folder)].view());
    }
}

void HierarchyPanel::move_to_folder(const flecs::world& ecs, CommandStack& commands,
                                    std::span<const flecs::entity_t> entities, std::string_view path) {
    const flecs::id_t component = ecs.component<EditorFolder>().id();
    char clean[128];
    clean_path(path, clean);
    commands.begin_group();
    for (const flecs::entity_t id : entities) {
        if (!ecs.is_alive(id)) {
            continue;
        }
        const flecs::entity entity = ecs.entity(id);
        if (!entity.has<EditorFolder>()) {
            entity.set(EditorFolder{}); // in no folder, as its file says
        }
        EditorFolder folder = entity.get<EditorFolder>();
        if (std::strcmp(folder.path, clean) == 0) {
            continue;
        }
        static_cast<void>(std::snprintf(folder.path, sizeof(folder.path), "%s", clean));
        static_cast<void>(commands.set_component(entity, component, std::as_bytes(std::span(&folder, 1))));
    }
    commands.end_group();
}

void HierarchyPanel::rename_folder(const flecs::world& ecs, CommandStack& commands, std::string_view from,
                                   std::string_view to) {
    char clean[128];
    clean_path(to, clean);
    const std::string_view into(clean);
    if (from.empty() || into == from || within(into, from)) {
        return; // nothing to do, or into itself
    }
    const flecs::id_t component = ecs.component<EditorFolder>().id();
    core::Array<flecs::entity_t> inside(core::heap_allocator(core::MemoryTag::Editor));
    ecs.each([&](flecs::entity entity, const EditorFolder& folder) {
        if (within(folder.path, from)) {
            inside.push(entity.id());
        }
    });
    commands.begin_group();
    for (const flecs::entity_t id : inside) {
        const flecs::entity entity = ecs.entity(id);
        EditorFolder folder = entity.get<EditorFolder>();
        const std::string_view rest = std::string_view(folder.path).substr(from.size());
        char path[256];
        const int length = std::snprintf(path, sizeof(path), "%.*s%.*s", static_cast<int>(into.size()), into.data(),
                                         static_cast<int>(rest.size()), rest.data());
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(folder.path)) {
            continue;
        }
        // Out of every folder ("" with a rest of "/a"): no slash in front.
        const char* start = path[0] == '/' ? path + 1 : path;
        static_cast<void>(std::snprintf(folder.path, sizeof(folder.path), "%s", start));
        static_cast<void>(commands.set_component(entity, component, std::as_bytes(std::span(&folder, 1))));
    }
    commands.end_group();
}

void HierarchyPanel::chosen_rows(const app::Engine& engine, const Selection& selection,
                                 core::Array<flecs::entity_t>& out) const {
    out.clear();
    if (selection.kind != Selection::Kind::Entity) {
        return;
    }
    const flecs::world& ecs = engine.world.ecs();
    for (const flecs::entity_t id : selection.entities) {
        if (id != hidden_ && ecs.is_alive(id) && ecs.entity(id).has<world::Transform>() &&
            !ecs.entity(id).has<world::Cell>()) {
            out.push(id);
        }
    }
}

void HierarchyPanel::start_rename(std::string_view path) {
    static_cast<void>(renaming_.assign(path));
    const std::string_view leaf = leaf_of(path);
    static_cast<void>(
        std::snprintf(rename_text_, sizeof(rename_text_), "%.*s", static_cast<int>(leaf.size()), leaf.data()));
    rename_focus_ = true;
    rows_stale_ = true;
}

void HierarchyPanel::new_folder(const app::Engine& engine, const Selection& selection, CommandStack& commands) {
    core::Array<flecs::entity_t> chosen(core::heap_allocator(core::MemoryTag::Editor));
    chosen_rows(engine, selection, chosen);
    if (chosen.empty()) {
        return;
    }
    // Beside the primary's folder (in it), named apart from the folders there are.
    const flecs::world& ecs = engine.world.ecs();
    const std::string_view beside =
        ecs.is_alive(selection.entity) ? std::string_view(folder_of(ecs.entity(selection.entity))) : std::string_view();
    char path[256];
    for (int n = 1; n < 1000; ++n) {
        const int length =
            n == 1 ? std::snprintf(path, sizeof(path), "%.*s%sNew Folder", static_cast<int>(beside.size()),
                                   beside.data(), beside.empty() ? "" : "/")
                   : std::snprintf(path, sizeof(path), "%.*s%sNew Folder %d", static_cast<int>(beside.size()),
                                   beside.data(), beside.empty() ? "" : "/", n);
        const std::string_view candidate(path, static_cast<size_t>(std::max(length, 0)));
        if (std::ranges::none_of(folders_, [candidate](const FolderPath& had) { return had.view() == candidate; })) {
            break;
        }
    }
    move_to_folder(engine.world.ecs(), commands, chosen.span(), path);
    start_rename(path);
}

void HierarchyPanel::context_menu(const app::Engine& engine, Selection& selection, CommandStack& commands) {
    push_menu_style();
    prepare_menu_popup();
    if (ImGui::BeginPopup(kMenu)) {
        core::Array<flecs::entity_t> chosen(core::heap_allocator(core::MemoryTag::Editor));
        chosen_rows(engine, selection, chosen);
        if (ImGui::MenuItem("Move to New Folder", nullptr, false, !chosen.empty())) {
            new_folder(engine, selection, commands);
        }
        if (ImGui::MenuItem("Move Out of Folders", nullptr, false, !chosen.empty())) {
            move_to_folder(engine.world.ecs(), commands, chosen.span(), "");
            rows_stale_ = true;
        }
        if (menu_folder_ >= 0 && static_cast<size_t>(menu_folder_) < folders_.size()) {
            const FolderPath folder = folders_[static_cast<size_t>(menu_folder_)];
            ImGui::Separator();
            if (ImGui::MenuItem("Rename", "F2")) {
                start_rename(folder.view());
            }
            if (ImGui::MenuItem("Remove Folder")) {
                // What's in it moves up a level; nothing is deleted.
                rename_folder(engine.world.ecs(), commands, folder.view(), parent_of(folder.view()));
                rows_stale_ = true;
            }
        }
        ImGui::EndPopup();
    }
    pop_menu_style();
}

void HierarchyPanel::draw(const app::Engine& engine, Selection& selection, flecs::entity_t hidden,
                          CommandStack& commands) {
    DE_PROFILE_ZONE("HierarchyPanel::draw");
    hidden_ = hidden;
    if (!ImGui::Begin(kWindow)) {
        focused_ = false;
        ImGui::End();
        return;
    }
    const flecs::world& ecs = engine.world.ecs();
    ImGui::SetCursorPos(ImVec2(px(6.0f), ImGui::GetCursorStartPos().y + px(2.0f)));
    if (search_field("##search", filter_, sizeof(filter_), "Search...", ImGui::GetContentRegionAvail().x - px(6.0f))) {
        rows_stale_ = true;
    }
    ImGui::Dummy(ImVec2(0.0f, px(4.0f)));
    if (const uint64_t now = platform::time_ns(); now >= next_read_ns_) {
        outline_.rebuild(ecs);
        next_read_ns_ = now + kReadEveryNs;
        rows_stale_ = true;
    }
    if (selection.kind == Selection::Kind::Entity && selection.entity != revealed_ && ecs.is_alive(selection.entity)) {
        revealed_ = selection.entity;
        reveal(ecs, selection.entity);
    }
    if (rows_stale_) {
        build_rows(engine, hidden);
        rows_stale_ = false;
    }

    const float row = row_height();
    const float footer = ImGui::GetTextLineHeight() + px(8.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, color(ThemeColor::Surface2));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    if (ImGui::BeginChild("##rows", ImVec2(0.0f, -footer), ImGuiChildFlags_None)) {
        const float width = ImGui::GetContentRegionAvail().x;
        if (scroll_to_ != 0) {
            // Into view, if it's out of it.
            for (size_t i = 0; i < rows_.size(); ++i) {
                if (rows_[i].entity == scroll_to_) {
                    const float y = static_cast<float>(i) * row;
                    const float top = ImGui::GetScrollY();
                    const float height = ImGui::GetWindowHeight();
                    if (y < top || y + row > top + height) {
                        ImGui::SetScrollY(std::max(0.0f, y - (height * 0.5f)));
                    }
                    break;
                }
            }
            scroll_to_ = 0;
        }
        char label[128];
        bool menu_asked = false;
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows_.size()), row);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const Row entry = rows_[static_cast<size_t>(i)];
                const bool is_folder = entry.folder >= 0;
                const FolderPath folder = is_folder ? folders_[static_cast<size_t>(entry.folder)] : FolderPath();
                const bool alive = !is_folder && ecs.is_alive(entry.entity);
                const char* name = "(gone)";
                if (is_folder) {
                    static_cast<void>(std::snprintf(label, sizeof(label), "%.*s",
                                                    static_cast<int>(leaf_of(folder.view()).size()),
                                                    leaf_of(folder.view()).data()));
                    name = label;
                } else if (alive) {
                    entity_label(engine.assets, ecs.entity(entry.entity), label);
                    name = label;
                }
                const bool selected =
                    is_folder ? chosen_folder_ == folder_key(folder.view()) && selection.kind == Selection::Kind::Entity
                              : selection.has_entity(entry.entity);
                ImGui::PushID(i);
                const ImVec2 min = ImGui::GetCursorScreenPos();
                const ImVec2 max(min.x + width, min.y + row);
                // The whole row chooses; the disclosure triangle drawn over it takes its own clicks.
                ImGui::SetNextItemAllowOverlap();
                const ImGuiIO& io = ImGui::GetIO();
                if (ImGui::InvisibleButton("##row", ImVec2(width, row)) && (alive || is_folder)) {
                    if (is_folder) {
                        // A folder: everything in it (Ctrl: added to what's chosen).
                        choose_folder(selection, selection, folder.view(), io.KeyCtrl);
                        chosen_folder_ = folder_key(folder.view());
                        revealed_ = selection.entity;
                    } else {
                        // Ctrl: this one in or out; Shift: the rows from the one chosen last to this; else only this.
                        chosen_folder_ = 0;
                        if (io.KeyCtrl) {
                            selection.toggle_entity(entry.entity);
                        } else if (io.KeyShift && selection.kind == Selection::Kind::Entity && selection.entity != 0) {
                            choose_range(selection, selection.entity, entry.entity);
                        } else {
                            selection.select_entity(entry.entity);
                        }
                        revealed_ = entry.entity; // it's in view already
                    }
                }
                const bool hovered = ImGui::IsItemHovered();
                if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (is_folder) {
                        start_rename(folder.view());
                    } else if (entry.has_children) {
                        set_open(entry, !is_open(entry));
                    } else if (alive) {
                        selection.frame_entity = true;
                    }
                }
                // The right button: the menu, for this row (chosen first, if it wasn't).
                if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    menu_folder_ = is_folder ? entry.folder : -1;
                    if (alive && !selection.has_entity(entry.entity)) {
                        selection.select_entity(entry.entity);
                        chosen_folder_ = 0;
                    }
                    menu_asked = true;
                }
                // Dragged: what's chosen (this row chosen first if it wasn't), or the folder.
                if ((alive || is_folder) && ImGui::BeginDragDropSource()) {
                    if (is_folder) {
                        ImGui::SetDragDropPayload(kFolderPayload, folder.c_str(), folder.view().size() + 1);
                        ImGui::Text("%s", name);
                    } else {
                        if (!selection.has_entity(entry.entity)) {
                            selection.select_entity(entry.entity);
                        }
                        ImGui::SetDragDropPayload(kRowsPayload, nullptr, 0);
                        if (selection.entities.size() == 1) {
                            ImGui::Text("%s", name);
                        } else {
                            ImGui::Text("%zu things", selection.entities.size());
                        }
                    }
                    ImGui::EndDragDropSource();
                }
                // Let go on a folder: into it.
                if (is_folder && ImGui::BeginDragDropTarget()) {
                    if (ImGui::AcceptDragDropPayload(kRowsPayload) != nullptr) {
                        core::Array<flecs::entity_t> chosen(core::heap_allocator(core::MemoryTag::Editor));
                        chosen_rows(engine, selection, chosen);
                        move_to_folder(engine.world.ecs(), commands, chosen.span(), folder.view());
                        rows_stale_ = true;
                    } else if (const ImGuiPayload* dropped = ImGui::AcceptDragDropPayload(kFolderPayload)) {
                        const std::string_view moved(static_cast<const char*>(dropped->Data));
                        char into[256];
                        static_cast<void>(std::snprintf(into, sizeof(into), "%s/%.*s", folder.c_str(),
                                                        static_cast<int>(leaf_of(moved).size()),
                                                        leaf_of(moved).data()));
                        rename_folder(engine.world.ecs(), commands, moved, into);
                        rows_stale_ = true;
                    }
                    ImGui::EndDragDropTarget();
                }
                // What it is, and where, on a pause over it.
                if (alive && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                    const flecs::entity entity = ecs.entity(entry.entity);
                    const flecs::entity cell = entity.target<world::InCell>();
                    if (const auto* coord = cell.is_valid() ? cell.try_get<world::Cell>() : nullptr) {
                        tooltip("%s in cell [%d, %d]", entity_kind(entity), coord->coord.x, coord->coord.z);
                    } else {
                        tooltip("%s", entity_kind(entity));
                    }
                }
                ImDrawList* draw = ImGui::GetWindowDrawList();
                if (selected) {
                    draw->AddRectFilled(min, max, color_u32(ThemeColor::Selection));
                } else if (hovered) {
                    draw->AddRectFilled(min, max, color_u32(ThemeColor::Header));
                }
                // Indentation, the disclosure triangle, a folder's glyph, the name (or its edit).
                const float indent = min.x + px(4.0f) + (px(14.0f) * static_cast<float>(entry.depth));
                if (entry.has_children) {
                    ImGui::SetCursorScreenPos(ImVec2(indent, min.y));
                    if (ImGui::InvisibleButton("##expand", ImVec2(px(14.0f), row))) {
                        set_open(entry, !is_open(entry));
                    }
                    draw_disclosure(draw, ImVec2(indent + px(6.0f), min.y + (row * 0.5f)), px(3.0f), is_open(entry),
                                    color_u32(ThemeColor::TextSecondary));
                }
                float name_x = indent + px(16.0f);
                if (is_folder) {
                    const float glyph = std::floor(row * 0.62f);
                    draw_folder_glyph(ImVec2(name_x, std::floor(min.y + ((row - glyph) * 0.5f))), glyph, glyph * 0.82f);
                    name_x += glyph + px(5.0f);
                }
                if (is_folder && renaming_.view() == folder.view()) {
                    ImGui::SetCursorScreenPos(ImVec2(name_x, min.y));
                    ImGui::SetNextItemWidth(std::max(px(60.0f), max.x - name_x - px(4.0f)));
                    if (rename_focus_) {
                        ImGui::SetKeyboardFocusHere();
                        rename_focus_ = false;
                    }
                    const bool done =
                        ImGui::InputText("##rename", rename_text_, sizeof(rename_text_),
                                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                        renaming_ = {};
                        rows_stale_ = true;
                    } else if (done || ImGui::IsItemDeactivated()) {
                        char renamed[256];
                        const std::string_view parent = parent_of(folder.view());
                        static_cast<void>(std::snprintf(renamed, sizeof(renamed), "%.*s%s%s",
                                                        static_cast<int>(parent.size()), parent.data(),
                                                        parent.empty() ? "" : "/", rename_text_));
                        if (rename_text_[0] != '\0') {
                            rename_folder(engine.world.ecs(), commands, folder.view(), renamed);
                        }
                        renaming_ = {};
                        rows_stale_ = true;
                    }
                } else {
                    const float text_y = std::floor(min.y + ((row - ImGui::GetFontSize()) * 0.5f));
                    draw->PushClipRect(ImVec2(name_x, min.y), ImVec2(max.x - px(4.0f), max.y), true);
                    draw->AddText(ImVec2(name_x, text_y),
                                  color_u32(selected ? ThemeColor::TextEmphasis : ThemeColor::Text), name);
                    draw->PopClipRect();
                }
                ImGui::SetCursorScreenPos(ImVec2(min.x, max.y));
                ImGui::PopID();
            }
        }
        clipper.End();
        // Below the rows: rows let go here go out of every folder, a folder to the top; a click chooses nothing.
        const float below = std::max(row, ImGui::GetContentRegionAvail().y);
        if (ImGui::InvisibleButton("##below", ImVec2(width, below))) {
            selection.select_entity(0);
            chosen_folder_ = 0;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            menu_folder_ = -1;
            menu_asked = true;
        }
        if (ImGui::BeginDragDropTarget()) {
            if (ImGui::AcceptDragDropPayload(kRowsPayload) != nullptr) {
                core::Array<flecs::entity_t> chosen(core::heap_allocator(core::MemoryTag::Editor));
                chosen_rows(engine, selection, chosen);
                move_to_folder(engine.world.ecs(), commands, chosen.span(), "");
                rows_stale_ = true;
            } else if (const ImGuiPayload* dropped = ImGui::AcceptDragDropPayload(kFolderPayload)) {
                const std::string_view moved(static_cast<const char*>(dropped->Data));
                rename_folder(engine.world.ecs(), commands, moved, leaf_of(moved));
                rows_stale_ = true;
            }
            ImGui::EndDragDropTarget();
        }
        if (menu_asked) {
            ImGui::OpenPopup(kMenu);
        }
        context_menu(engine, selection, commands);
        keyboard(selection);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // How many rows the tree shows.
    static_cast<void>(
        std::snprintf(status_, sizeof(status_), "%zu item%s", rows_.size(), rows_.size() == 1 ? "" : "s"));
    ImGui::SetCursorPosX(px(8.0f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(4.0f));
    ImGui::TextColored(color(ThemeColor::TextSecondary), "%s", status_);
    focused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();
}

void HierarchyPanel::choose_range(Selection& selection, flecs::entity_t from, flecs::entity_t to) const {
    // The rows as listed, from one to the other (either way round): the one clicked ends up the primary.
    size_t first = rows_.size();
    size_t last = rows_.size();
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].entity == from) {
            first = i;
        }
        if (rows_[i].entity == to) {
            last = i;
        }
    }
    if (first == rows_.size() || last == rows_.size()) {
        selection.add_entity(to);
        return;
    }
    const size_t low = std::min(first, last);
    const size_t high = std::max(first, last);
    for (size_t i = low; i <= high; ++i) {
        if (rows_[i].entity != to && rows_[i].entity != 0) {
            selection.add_entity(rows_[i].entity);
        }
    }
    selection.add_entity(to);
}

} // namespace de::editor
