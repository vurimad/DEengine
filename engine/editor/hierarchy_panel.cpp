#include "editor/hierarchy_panel.h"

#include "app/framework.h"
#include "core/profile.h"
#include "editor/theme.h"
#include "editor/widgets.h"
#include "platform/time.h"
#include "world/cell.h"
#include "world/world.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace de::editor {

namespace {

constexpr uint64_t kReadEveryNs = platform::kNsPerSecond / 4;
constexpr size_t kMaxMatches = 5000; // a search shows at most this many

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool name_before(std::string_view a, std::string_view b) {
    return std::ranges::lexicographical_compare(a, b, [](char x, char y) { return lower(x) < lower(y); });
}

} // namespace

void HierarchyPanel::reset() {
    open_.clear();
    rows_.clear();
    roots_.clear();
    next_read_ns_ = 0;
    rows_stale_ = true;
    revealed_ = 0;
}

void HierarchyPanel::set_open(const Row& row, bool open) {
    if (!row.has_children || is_open(row) == open) {
        return;
    }
    if (open) {
        open_.set(row.entity, 1);
    } else {
        static_cast<void>(open_.remove(row.entity));
    }
    rows_stale_ = true;
}

void HierarchyPanel::reveal(const flecs::world& ecs, flecs::entity_t entity) {
    // Every parent up from it opens.
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
    // What's attached to nothing, by name; under each, what's attached to it.
    roots_.clear();
    for (const Outline::Item& item : outline_.roots()) {
        if (item.entity == hidden) {
            continue;
        }
        entity_label(engine.assets, ecs.entity(item.entity), label);
        Root& root = roots_.push({.entity = item.entity, .name = {}});
        static_cast<void>(root.name.assign(label));
    }
    std::ranges::sort(roots_, [](const Root& a, const Root& b) { return name_before(a.name.view(), b.name.view()); });
    for (const Root& root : roots_) {
        const std::span<const Outline::Item> children = outline_.children(root.entity);
        rows_.push({.entity = root.entity, .depth = 0, .has_children = !children.empty()});
        if (!children.empty() && open_.contains(root.entity)) {
            add_items(children, 1);
        }
    }
}

void HierarchyPanel::keyboard(Selection& selection) {
    if (!ImGui::IsWindowFocused() || ImGui::IsAnyItemActive() || rows_.empty()) {
        return;
    }
    size_t at = rows_.size();
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (selection.kind == Selection::Kind::Entity && rows_[i].entity == selection.entity) {
            at = i;
            break;
        }
    }
    const auto select = [&](size_t index) {
        if (index < rows_.size()) {
            selection.select_entity(rows_[index].entity);
            revealed_ = rows_[index].entity;
            scroll_to_ = rows_[index].entity;
        }
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
    }
}

void HierarchyPanel::draw(const app::Engine& engine, Selection& selection, flecs::entity_t hidden) {
    DE_PROFILE_ZONE("HierarchyPanel::draw");
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
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows_.size()), row);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const Row& entry = rows_[static_cast<size_t>(i)];
                const bool alive = ecs.is_alive(entry.entity);
                const char* name = "(gone)";
                if (alive) {
                    entity_label(engine.assets, ecs.entity(entry.entity), label);
                    name = label;
                }
                const bool selected = selection.has_entity(entry.entity);
                ImGui::PushID(i);
                const ImVec2 min = ImGui::GetCursorScreenPos();
                const ImVec2 max(min.x + width, min.y + row);
                // The whole row selects; the disclosure triangle drawn over it takes its own clicks.
                ImGui::SetNextItemAllowOverlap();
                if (ImGui::InvisibleButton("##row", ImVec2(width, row)) && alive) {
                    // Ctrl: this one in or out; Shift: the rows from the one chosen last to this; else only this.
                    const ImGuiIO& io = ImGui::GetIO();
                    if (io.KeyCtrl) {
                        selection.toggle_entity(entry.entity);
                    } else if (io.KeyShift && selection.kind == Selection::Kind::Entity && selection.entity != 0) {
                        choose_range(selection, selection.entity, entry.entity);
                    } else {
                        selection.select_entity(entry.entity);
                    }
                    revealed_ = entry.entity; // it's in view already
                }
                const bool hovered = ImGui::IsItemHovered();
                if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (entry.has_children) {
                        set_open(entry, !is_open(entry));
                    } else if (alive) {
                        selection.frame_entity = true;
                    }
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
                // Indentation, the disclosure triangle, the name.
                const float indent = min.x + px(4.0f) + (px(14.0f) * static_cast<float>(entry.depth));
                if (entry.has_children) {
                    ImGui::SetCursorScreenPos(ImVec2(indent, min.y));
                    if (ImGui::InvisibleButton("##expand", ImVec2(px(14.0f), row))) {
                        set_open(entry, !is_open(entry));
                    }
                    draw_disclosure(draw, ImVec2(indent + px(6.0f), min.y + (row * 0.5f)), px(3.0f), is_open(entry),
                                    color_u32(ThemeColor::TextSecondary));
                }
                const float text_y = std::floor(min.y + ((row - ImGui::GetFontSize()) * 0.5f));
                const float name_x = indent + px(16.0f);
                draw->PushClipRect(ImVec2(name_x, min.y), ImVec2(max.x - px(4.0f), max.y), true);
                draw->AddText(ImVec2(name_x, text_y), color_u32(selected ? ThemeColor::TextEmphasis : ThemeColor::Text),
                              name);
                draw->PopClipRect();
                ImGui::SetCursorScreenPos(ImVec2(min.x, max.y));
                ImGui::PopID();
            }
        }
        clipper.End();
        keyboard(selection);
        // A click on empty space clears the selection.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered()) {
            selection.select_entity(0);
        }
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
        if (rows_[i].entity != to) {
            selection.add_entity(rows_[i].entity);
        }
    }
    selection.add_entity(to);
}

} // namespace de::editor
