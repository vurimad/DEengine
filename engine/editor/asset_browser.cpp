#include "editor/asset_browser.h"

#include "core/hash.h"
#include "core/log.h"
#include "core/profile.h"
#include "core/text.h"
#include "editor/outline.h"
#include "editor/source_files.h"
#include "editor/theme.h"
#include "editor/widgets.h"
#include "platform/vfs.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

namespace de::editor {

namespace {

constexpr std::string_view kRoot = "src:/content";
constexpr const char* kSettingsPath = "user:/editor/resources.settings";
constexpr size_t kMaxMatches = 2000;
constexpr float kGridMin = 48.0f;
constexpr float kGridMax = 160.0f;
// Room before a tree row's label for its glyph.
constexpr const char* kLabelInset = "       ";
// The tree's highlight of the folders above the one shown: a faint selection.
constexpr float kAncestorAlpha = 0.28f;

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool name_before(std::string_view a, std::string_view b) {
    return std::ranges::lexicographical_compare(a, b, [](char x, char y) { return lower(x) < lower(y); });
}

void inline_folder_glyph(ImVec2 label, float row) {
    const float reserved = ImGui::CalcTextSize(kLabelInset).x;
    const float width = std::max(0.0f, std::min(reserved - px(6.0f), row * 0.95f));
    const float height = width * 0.64f;
    draw_folder_glyph(ImVec2(label.x, label.y + std::max(0.0f, (row - height) * 0.5f)), width, height);
}

void inline_document_glyph(ImVec2 label, float row, const char* name) {
    const float reserved = ImGui::CalcTextSize(kLabelInset).x;
    const float extent = std::max(0.0f, std::min(reserved - px(6.0f), row * 0.88f));
    draw_document_glyph(ImVec2(label.x, label.y + std::max(0.0f, (row - extent) * 0.5f)), extent, name, false);
}

// A leaf's tree line reaches its glyph, as a branch's does.
void leaf_connector(ImVec2 item_min, ImVec2 item_max, float content_x) {
    const ImGuiStyle& style = ImGui::GetStyle();
    if ((style.TreeLinesFlags & (ImGuiTreeNodeFlags_DrawLinesFull | ImGuiTreeNodeFlags_DrawLinesToNodes)) == 0 ||
        style.TreeLinesSize <= 0.0f) {
        return;
    }
    const float x1 = item_min.x + style.FramePadding.x;
    const float x2 = content_x - style.ItemInnerSpacing.x;
    if (x2 <= x1) {
        return;
    }
    const float y = (item_min.y + item_max.y) * 0.5f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x1, y), ImVec2(x2, y), ImGui::GetColorU32(ImGuiCol_TreeLines),
                                        style.TreeLinesSize);
}

// A path for people: "Content/terrain/layers".
void display_path(std::string_view path, char* out, size_t capacity) {
    const std::string_view rest = path.starts_with(kRoot) ? path.substr(kRoot.size()) : path;
    static_cast<void>(std::snprintf(out, capacity, "Content%.*s", static_cast<int>(rest.size()), rest.data()));
}

} // namespace

AssetBrowser::AssetBrowser(core::Allocator& allocator)
    : names_(allocator), entries_(allocator), shown_(allocator), selected_(allocator), back_(allocator),
      forward_(allocator), favorites_(allocator), tree_open_(allocator) {}

std::string_view AssetBrowser::name_of(uint32_t entry) const {
    if (entry == 0) {
        return "Content";
    }
    const Entry& e = entries_[entry];
    return {names_.data() + e.name, e.name_length};
}

bool AssetBrowser::path_of(uint32_t entry, Path& out) const {
    // Up to the root, then back down.
    uint32_t chain[32];
    uint32_t depth = 0;
    for (uint32_t at = entry; at != 0 && depth < 32; at = entries_[at].parent) {
        chain[depth++] = at;
    }
    bool fits = out.assign(kRoot);
    while (depth > 0) {
        fits = fits && out.append('/') && out.append(name_of(chain[--depth]));
    }
    return fits;
}

uint32_t AssetBrowser::find(std::string_view path) const {
    Path candidate;
    for (uint32_t i = 0; i < entries_.size(); ++i) {
        if (path_of(i, candidate) && candidate.view() == path) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool AssetBrowser::has_child_folders(uint32_t folder) const {
    const Entry& entry = entries_[folder];
    return entry.child_count > 0 && entries_[entry.first_child].directory; // folders come first
}

void AssetBrowser::scan(const platform::Vfs& vfs) {
    DE_PROFILE_ZONE("AssetBrowser::scan");
    Path shown_path;
    const bool had = scanned_ && current_ < entries_.size() && path_of(current_, shown_path);
    names_.clear();
    entries_.clear();
    selected_.clear();
    back_.clear();
    forward_.clear();
    anchor_ = UINT32_MAX;
    entries_.push({.key = core::fnv1a_64(kRoot.data(), kRoot.size()), .directory = true});
    Path path;
    // Folder by folder, in the order they're found: each one's entries land together.
    for (uint32_t folder = 0; folder < entries_.size(); ++folder) {
        if (!entries_[folder].directory || !path_of(folder, path)) {
            continue;
        }
        const auto first = static_cast<uint32_t>(entries_.size());
        const auto listed = vfs.list(path.c_str(), [&](const char* name, bool is_directory) {
            if (name[0] == '.') {
                return; // hidden
            }
            const size_t length = std::strlen(name);
            Path child = path;
            const bool fits = child.append('/') && child.append(std::string_view(name, length));
            entries_.push({.name = static_cast<uint32_t>(names_.size()),
                           .name_length = static_cast<uint32_t>(length),
                           .parent = folder,
                           .key = fits ? core::fnv1a_64(child.c_str(), child.size()) : 0,
                           .directory = is_directory});
            names_.resize(names_.size() + length);
            std::memcpy(names_.data() + names_.size() - length, name, length);
        });
        if (!listed) {
            DE_LOG_WARN("editor", "can't list %s: %s", path.c_str(), listed.error().message);
        }
        std::sort(entries_.begin() + first, entries_.end(), [this](const Entry& a, const Entry& b) {
            if (a.directory != b.directory) {
                return a.directory;
            }
            return name_before({names_.data() + a.name, a.name_length}, {names_.data() + b.name, b.name_length});
        });
        entries_[folder].first_child = first;
        entries_[folder].child_count = static_cast<uint32_t>(entries_.size()) - first;
    }
    tree_open_.set(entries_[0].key, 1); // the root starts open
    const uint32_t found = had ? find(shown_path.view()) : UINT32_MAX;
    current_ = found != UINT32_MAX && entries_[found].directory ? found : 0;
    reveal(current_);
    scanned_ = true;
}

void AssetBrowser::reveal(uint32_t entry) {
    for (uint32_t at = entries_[entry].parent; at != 0; at = entries_[at].parent) {
        tree_open_.set(entries_[at].key, 1);
    }
    tree_open_.set(entries_[0].key, 1);
}

void AssetBrowser::navigate(uint32_t folder, bool remember) {
    if (folder >= entries_.size() || !entries_[folder].directory) {
        return;
    }
    search_[0] = '\0';
    if (folder == current_) {
        return;
    }
    if (remember) {
        back_.push(current_);
        forward_.clear();
    }
    current_ = folder;
    selected_.clear();
    anchor_ = UINT32_MAX;
    reveal(folder);
    scroll_to_top_ = true;
}

bool AssetBrowser::is_selected(uint32_t entry) const {
    return std::ranges::find(selected_, entry) != selected_.end();
}

AssetBrowser::SelectMode AssetBrowser::select_mode() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyShift) {
        return io.KeyCtrl ? SelectMode::AddRange : SelectMode::Range;
    }
    return io.KeyCtrl ? SelectMode::Toggle : SelectMode::Replace;
}

void AssetBrowser::select(uint32_t entry, SelectMode mode, Selection& selection) {
    const auto position = [this](uint32_t item) {
        const auto* at = std::ranges::find(shown_, item);
        return at == shown_.end() ? SIZE_MAX : static_cast<size_t>(at - shown_.begin());
    };
    const size_t from = anchor_ != UINT32_MAX ? position(anchor_) : SIZE_MAX;
    const size_t to = position(entry);
    if ((mode == SelectMode::Range || mode == SelectMode::AddRange) && from != SIZE_MAX && to != SIZE_MAX) {
        if (mode == SelectMode::Range) {
            selected_.clear();
        }
        for (size_t i = std::min(from, to); i <= std::max(from, to); ++i) {
            if (!is_selected(shown_[i])) {
                selected_.push(shown_[i]);
            }
        }
    } else if (mode == SelectMode::Toggle) {
        if (const auto* at = std::ranges::find(selected_, entry); at != selected_.end()) {
            selected_.remove_ordered(static_cast<size_t>(at - selected_.begin()));
        } else {
            selected_.push(entry);
        }
        anchor_ = entry;
    } else {
        selected_.clear();
        selected_.push(entry);
        anchor_ = entry;
    }
    // A file shows in the properties.
    Path path;
    if (!entries_[entry].directory && is_selected(entry) && path_of(entry, path)) {
        selection.select_file(path.view());
    }
}

void AssetBrowser::open_item(uint32_t entry, Selection& selection) {
    if (entries_[entry].directory) {
        navigate(entry);
        return;
    }
    Path path;
    if (path_of(entry, path)) {
        selection.select_file(path.view());
        opened_ = entry;
    }
}

void AssetBrowser::rebuild_shown() {
    shown_.clear();
    const std::string_view search(search_);
    if (search.empty()) {
        const Entry& folder = entries_[current_];
        for (uint32_t i = 0; i < folder.child_count; ++i) {
            shown_.push(folder.first_child + i);
        }
        return;
    }
    for (uint32_t i = 1; i < entries_.size() && shown_.size() < kMaxMatches; ++i) {
        if (contains_ignoring_case(name_of(i), search)) {
            shown_.push(i);
        }
    }
}

// ---- Toolbar ----

bool AssetBrowser::nav_button(const char* id, NavIcon icon, const char* tip) {
    const float extent = ImGui::GetFrameHeight();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    const bool pressed = ImGui::InvisibleButton("##navigation", ImVec2(extent, extent));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImGui::PopID();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered || held) {
        draw->AddRectFilled(min, ImVec2(min.x + extent, min.y + extent),
                            ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered));
    }
    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
    const ImVec2 center(min.x + (extent * 0.5f), min.y + (extent * 0.5f));
    const float radius = extent * 0.27f;
    const float line = std::max(px(1.15f), extent * (hovered || held ? 0.075f : 0.062f));
    if (icon == NavIcon::Refresh) {
        draw_circular_arrow(draw, center, radius, 0.9f, 4.75f, ink, line);
    } else {
        const float direction = icon == NavIcon::Back ? -1.0f : 1.0f;
        const float tip_x = center.x + (direction * radius * 0.72f);
        const float tail_x = center.x - (direction * radius * 0.78f);
        draw->AddLine(ImVec2(tail_x, center.y), ImVec2(tip_x, center.y), ink, line);
        draw->AddLine(ImVec2(tip_x, center.y),
                      ImVec2(tip_x - (direction * radius * 0.78f), center.y - (radius * 0.72f)), ink, line);
        draw->AddLine(ImVec2(tip_x, center.y),
                      ImVec2(tip_x - (direction * radius * 0.78f), center.y + (radius * 0.72f)), ink, line);
    }
    if (hovered) {
        tooltip("%s", tip);
    }
    return pressed;
}

void AssetBrowser::draw_breadcrumbs() {
    // A flat path: steps are buttons only on hover, the folder shown is semibold.
    const float height = ImGui::GetFrameHeight();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 bar_min = ImGui::GetCursorScreenPos();
    const float content_right = bar_min.x + width - px(4.0f);
    const float padding = px(6.0f);
    const float separator = px(12.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    uint32_t chain[32];
    uint32_t depth = 0;
    for (uint32_t at = current_; depth < 32; at = entries_[at].parent) {
        chain[depth++] = at;
        if (at == 0) {
            break;
        }
    }
    uint32_t target = UINT32_MAX;
    while (depth > 0) {
        const uint32_t step = chain[--depth];
        const bool current = step == current_;
        if (step != 0) {
            // The separator: a thin chevron.
            if (ImGui::GetCursorScreenPos().x + separator > content_right) {
                break;
            }
            ImGui::Dummy(ImVec2(separator, height));
            const ImVec2 at = ImGui::GetItemRectMin();
            const float x = at.x + (separator * 0.46f);
            const float y = at.y + (height * 0.5f);
            const float chevron = std::max(2.5f, height * 0.13f);
            const float line = std::max(1.0f, px(1.2f));
            const ImU32 ink = color_u32(ThemeColor::TextSecondary);
            draw->AddLine(ImVec2(x - (chevron * 0.45f), y - chevron), ImVec2(x + (chevron * 0.45f), y), ink, line);
            draw->AddLine(ImVec2(x + (chevron * 0.45f), y), ImVec2(x - (chevron * 0.45f), y + chevron), ink, line);
            ImGui::SameLine(0.0f, 0.0f);
        }
        const float remaining = content_right - ImGui::GetCursorScreenPos().x;
        if (remaining <= 4.0f) {
            break;
        }
        char label[128];
        const std::string_view name = name_of(step);
        static_cast<void>(std::snprintf(label, sizeof(label), "%.*s", static_cast<int>(name.size()), name.data()));
        ImFont* bold = current ? fonts().semibold : nullptr;
        if (bold != nullptr) {
            ImGui::PushFont(bold, 0.0f);
        }
        const float desired = ImGui::CalcTextSize(label).x + (padding * 2.0f);
        const float segment = std::max(1.0f, std::min(desired, remaining));
        const ImVec2 min = ImGui::GetCursorScreenPos();
        ImGui::PushID(static_cast<int>(step));
        const bool clicked = ImGui::InvisibleButton("##crumb", ImVec2(segment, height));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const ImVec2 max(min.x + segment, min.y + height);
        if (hovered && !current) {
            draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_ButtonHovered), ImGui::GetStyle().FrameRounding);
        }
        draw_clipped_label(ImVec2(min.x + padding, min.y), ImVec2(max.x - padding, max.y), label,
                           color_u32(current ? ThemeColor::TextEmphasis : ThemeColor::Text), false);
        if (bold != nullptr) {
            ImGui::PopFont();
        }
        if (hovered && desired > segment) {
            tooltip("%s", label);
        }
        if (clicked && !current) {
            target = step;
        }
        if (!current) {
            ImGui::SameLine(0.0f, 0.0f);
        }
    }
    if (target != UINT32_MAX) {
        navigate(target);
    }
}

void AssetBrowser::draw_favorites() {
    constexpr const char* kPopup = "##favorites_list";
    Path current_path;
    const bool have_path = path_of(current_, current_path);
    const auto favorite_at = [&](std::string_view path) {
        for (size_t i = 0; i < favorites_.size(); ++i) {
            if (favorites_[i].view() == path) {
                return i;
            }
        }
        return SIZE_MAX;
    };
    const size_t current_favorite = have_path ? favorite_at(current_path.view()) : SIZE_MAX;

    // One recessed field: [star | Favorites v]; the list opens under the whole field at its width.
    const float height = ImGui::GetFrameHeight();
    const float list_width = px(150.0f);
    const ImVec2 group_min = ImGui::GetCursorScreenPos();
    const float group_width = height + list_width;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    {
        // The star: filled when the folder shown is a favourite.
        const ImVec2 min = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##favorite_toggle", ImVec2(height, height)) && have_path) {
            if (current_favorite != SIZE_MAX) {
                favorites_.remove_ordered(current_favorite);
            } else {
                favorites_.push(current_path);
            }
        }
        const bool hovered = ImGui::IsItemHovered();
        const ImGuiCol background = ImGui::IsItemActive() ? ImGuiCol_ButtonActive
                                    : hovered             ? ImGuiCol_ButtonHovered
                                                          : ImGuiCol_FrameBg;
        draw->AddRectFilled(min, ImVec2(min.x + height, min.y + height), ImGui::GetColorU32(background),
                            ImGui::GetStyle().FrameRounding, ImDrawFlags_RoundCornersLeft);
        ImVec2 points[10];
        const ImVec2 center(min.x + (height * 0.5f), min.y + (height * 0.53f));
        const float outer = height * 0.32f;
        for (int i = 0; i < 10; ++i) {
            const float angle = -1.5707963f + (static_cast<float>(i) * 0.62831853f);
            const float radius = (i & 1) != 0 ? outer * 0.45f : outer;
            points[i] = ImVec2(center.x + (std::cos(angle) * radius), center.y + (std::sin(angle) * radius));
        }
        if (current_favorite != SIZE_MAX) {
            draw->AddConcavePolyFilled(points, 10, color_u32(ThemeColor::Warning));
        } else {
            draw->AddPolyline(points, 10, color_u32(hovered ? ThemeColor::TextEmphasis : ThemeColor::Text),
                              ImDrawFlags_Closed, std::max(1.0f, px(1.2f)));
        }
        if (hovered) {
            tooltip("%s", current_favorite != SIZE_MAX ? "Remove this folder from favorites"
                                                       : "Add this folder to favorites");
        }
    }
    ImGui::SameLine(0.0f, 0.0f);
    {
        const ImVec2 min = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##favorites", ImVec2(list_width, height))) {
            ImGui::OpenPopup(kPopup);
        }
        const bool hovered = ImGui::IsItemHovered();
        const bool open = ImGui::IsPopupOpen(kPopup);
        const ImGuiCol background = open      ? ImGuiCol_FrameBgActive
                                    : hovered ? ImGuiCol_FrameBgHovered
                                              : ImGuiCol_FrameBg;
        draw->AddRectFilled(min, ImVec2(min.x + list_width, min.y + height), ImGui::GetColorU32(background),
                            ImGui::GetStyle().FrameRounding, ImDrawFlags_RoundCornersRight);
        draw->AddText(
            ImVec2(min.x + (ImGui::GetStyle().FramePadding.x * 0.5f), min.y + ImGui::GetStyle().FramePadding.y),
            ImGui::GetColorU32(ImGuiCol_Text), "Favorites");
        draw_chevron(draw, ImVec2(min.x + list_width - (height * 0.5f), min.y + (height * 0.5f)), height * 0.16f,
                     ImGuiDir_Down, color_u32(ThemeColor::TextSecondary));
    }

    push_menu_style();
    ImGui::SetNextWindowPos(ImVec2(group_min.x, group_min.y + height + px(1.0f)));
    ImGui::SetNextWindowSizeConstraints(ImVec2(group_width, 0.0f), ImVec2(group_width, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, px(4.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    size_t remove = SIZE_MAX;
    uint32_t go = UINT32_MAX;
    if (ImGui::BeginPopup(kPopup)) {
        const float row = row_height() + px(2.0f);
        const float inset = px(10.0f);
        if (favorites_.empty()) {
            const ImVec2 min = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(group_width, row));
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(min.x + inset, std::floor(min.y + ((row - ImGui::GetFontSize()) * 0.5f))),
                color_u32(ThemeColor::TextSecondary), "No favorite folders");
        }
        for (size_t i = 0; i < favorites_.size(); ++i) {
            char label[160];
            display_path(favorites_[i].view(), label, sizeof(label));
            ImGui::PushID(static_cast<int>(i));
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const float remove_width = row;
            if (ImGui::Selectable("##favorite", have_path && favorites_[i].view() == current_path.view(),
                                  ImGuiSelectableFlags_AllowOverlap, ImVec2(group_width, row))) {
                go = find(favorites_[i].view());
            }
            ImDrawList* popup = ImGui::GetWindowDrawList();
            const float label_right = min.x + group_width - remove_width;
            const ImVec2 size = ImGui::CalcTextSize(label);
            const ImVec2 label_min(min.x + inset, std::floor(min.y + ((row - ImGui::GetFontSize()) * 0.5f)));
            ImGui::RenderTextEllipsis(popup, label_min, ImVec2(label_right, label_min.y + size.y), label_right, label,
                                      nullptr, &size);
            if (ImGui::IsItemHovered() && size.x > label_right - label_min.x) {
                tooltip("%s", label);
            }
            const ImVec2 remove_min(label_right, min.y);
            ImGui::SetCursorScreenPos(remove_min);
            if (ImGui::InvisibleButton("##remove", ImVec2(remove_width, row))) {
                remove = i;
            }
            const bool remove_hovered = ImGui::IsItemHovered();
            if (remove_hovered) {
                popup->AddRectFilled(remove_min, ImVec2(remove_min.x + remove_width, remove_min.y + row),
                                     ImGui::GetColorU32(ImGuiCol_ButtonActive));
            }
            const ImVec2 center(std::floor(remove_min.x + (remove_width * 0.5f)) + 0.5f,
                                std::floor(remove_min.y + (row * 0.5f)) + 0.5f);
            const float half = std::floor(px(3.5f));
            const ImU32 ink = color_u32(remove_hovered ? ThemeColor::TextEmphasis : ThemeColor::TextSecondary);
            const float line = std::floor(std::max(1.0f, px(1.0f)));
            popup->AddLine(ImVec2(center.x - half, center.y - half), ImVec2(center.x + half, center.y + half), ink,
                           line);
            popup->AddLine(ImVec2(center.x + half, center.y - half), ImVec2(center.x - half, center.y + half), ink,
                           line);
            if (remove_hovered) {
                tooltip("Remove from favorites");
            }
            ImGui::SetCursorScreenPos(ImVec2(min.x, min.y + row));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            ImGui::PopID();
        }
        if (go != UINT32_MAX) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    pop_menu_style();
    if (remove < favorites_.size()) {
        favorites_.remove_ordered(remove);
    }
    if (go != UINT32_MAX) {
        navigate(go);
    }
}

void AssetBrowser::draw_view_options() {
    constexpr const char* kPopup = "##view_options";
    const float extent = ImGui::GetFrameHeight();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##view_options_button", ImVec2(extent, extent))) {
        ImGui::OpenPopup(kPopup);
    }
    const bool hovered = ImGui::IsItemHovered();
    const bool open = ImGui::IsPopupOpen(kPopup);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered || open) {
        draw->AddRectFilled(min, ImVec2(min.x + extent, min.y + extent),
                            ImGui::GetColorU32(open ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered),
                            ImGui::GetStyle().FrameRounding);
    }
    // A gear: eight teeth around a ring.
    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
    const ImVec2 center(min.x + (extent * 0.5f), min.y + (extent * 0.5f));
    const float radius = extent * 0.2f;
    const float line = std::max(1.0f, px(1.3f));
    for (int tooth = 0; tooth < 8; ++tooth) {
        const float angle = static_cast<float>(tooth) * 0.78539816f;
        const ImVec2 direction(std::cos(angle), std::sin(angle));
        draw->AddLine(ImVec2(center.x + (direction.x * radius), center.y + (direction.y * radius)),
                      ImVec2(center.x + (direction.x * radius * 1.55f), center.y + (direction.y * radius * 1.55f)), ink,
                      line * 1.6f);
    }
    draw->AddCircle(center, radius, ink, 20, line);
    draw->AddCircle(center, radius * 0.4f, ink, 12, line);
    if (hovered && !open) {
        tooltip("View options");
    }
    push_menu_style();
    prepare_menu_popup();
    if (ImGui::BeginPopup(kPopup)) {
        menu_section("Layout");
        if (ImGui::MenuItem("Two columns", nullptr, layout_ == Layout::TwoColumns)) {
            layout_ = Layout::TwoColumns;
            reveal(current_);
        }
        if (ImGui::MenuItem("One column", nullptr, layout_ == Layout::OneColumn)) {
            layout_ = Layout::OneColumn;
            reveal(current_);
        }
        menu_section("Thumbnails");
        ImGui::SetNextItemWidth(px(200.0f));
        static_cast<void>(ImGui::SliderFloat("##grid_extent", &grid_extent_, kGridMin, kGridMax, "%.0f px"));
        if (ImGui::IsItemHovered()) {
            tooltip("Thumbnail size (Ctrl+Mouse Wheel over the grid)");
        }
        ImGui::EndPopup();
    }
    pop_menu_style();
}

void AssetBrowser::draw_toolbar() {
    const float row_padding = std::max(0.0f, (toolbar_height() - ImGui::GetFrameHeight()) * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(px(6.0f), row_padding));
    if (ImGui::BeginTable("##toolbar", 3, ImGuiTableFlags_SizingStretchProp)) {
        const float spacing = px(2.0f);
        const float button = ImGui::GetFrameHeight();
        const float group_spacing = px(12.0f);
        ImGui::TableSetupColumn("##navigation", ImGuiTableColumnFlags_WidthFixed, (button * 3.0f) + (spacing * 2.0f));
        ImGui::TableSetupColumn("##location", ImGuiTableColumnFlags_WidthStretch);
        // Ends where the contents' row below ends.
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed,
                                button + px(150.0f) + group_spacing + button + content_inset() +
                                    ImGui::GetStyle().CellPadding.x);
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(back_.empty());
        const bool back = nav_button("back", NavIcon::Back, "Back");
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, spacing);
        ImGui::BeginDisabled(forward_.empty());
        const bool forward = nav_button("forward", NavIcon::Forward, "Forward");
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, spacing);
        refresh_ = nav_button("refresh", NavIcon::Refresh, "Refresh") || refresh_;

        ImGui::TableNextColumn();
        // One column has no contents row: its search takes the path's place.
        if (layout_ == Layout::OneColumn) {
            static_cast<void>(search_field("##search", search_, sizeof(search_), "Search Content", 0.0f));
            if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                search_[0] = '\0';
            }
        } else {
            draw_breadcrumbs();
        }

        ImGui::TableNextColumn();
        draw_favorites();
        ImGui::SameLine(0.0f, group_spacing);
        draw_view_options();
        ImGui::EndTable();

        if (back && !back_.empty()) {
            forward_.push(current_);
            const uint32_t to = back_.back();
            back_.pop();
            navigate(to, false);
        } else if (forward && !forward_.empty()) {
            back_.push(current_);
            const uint32_t to = forward_.back();
            forward_.pop();
            navigate(to, false);
        }
    }
    ImGui::PopStyleVar();
}

// ---- The folder tree ----

void AssetBrowser::draw_tree(uint32_t folder, bool include_files, Selection& selection) {
    const Entry& entry = entries_[folder];
    const bool root = folder == 0;
    const bool branch = include_files ? entry.child_count > 0 : has_child_folders(folder);
    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanFullWidth;
    if (!branch) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    // The folder shown, and faintly the folders above it.
    bool ancestor = false;
    for (uint32_t at = current_; at != 0 && !ancestor; at = entries_[at].parent) {
        ancestor = entries_[at].parent == folder;
    }
    const bool selected = folder == current_;
    if (selected || ancestor) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    if (branch) {
        ImGui::SetNextItemOpen(tree_open_.contains(entry.key), ImGuiCond_Always);
    }
    if (ancestor) {
        ImVec4 header = ImGui::GetStyleColorVec4(ImGuiCol_Header);
        header.w *= kAncestorAlpha;
        ImGui::PushStyleColor(ImGuiCol_Header, header);
    }
    ImFont* root_font = root ? fonts().semibold : nullptr; // the root is the tree's heading
    if (root_font != nullptr) {
        ImGui::PushFont(root_font, 0.0f);
    }
    char label[128];
    const std::string_view name = name_of(folder);
    static_cast<void>(
        std::snprintf(label, sizeof(label), "%s%.*s", kLabelInset, static_cast<int>(name.size()), name.data()));
    ImGui::PushID(static_cast<int>(folder));
    // Rows span the full width to select, so glyphs go from the indented cursor, not the row.
    const float content_x = ImGui::GetCursorScreenPos().x;
    const bool open = ImGui::TreeNodeEx("##folder", flags, "%s", label);
    if (root_font != nullptr) {
        ImGui::PopFont();
    }
    if (ancestor) {
        ImGui::PopStyleColor();
    }
    const ImVec2 item_min = ImGui::GetItemRectMin();
    const ImVec2 item_max = ImGui::GetItemRectMax();
    const ImVec2 glyph(content_x + ImGui::GetTreeNodeToLabelSpacing(), item_min.y);
    if (!root && !branch) {
        leaf_connector(ImVec2(content_x, item_min.y), item_max, glyph.x);
    }
    inline_folder_glyph(glyph, item_max.y - item_min.y);
    const bool toggled = ImGui::IsItemToggledOpen();
    if (toggled && branch) {
        if (open) {
            tree_open_.set(entry.key, 1);
        } else {
            static_cast<void>(tree_open_.remove(entry.key));
        }
    }
    if ((ImGui::IsItemClicked(ImGuiMouseButton_Left) && !toggled) ||
        (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
        navigate(folder);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        context_entry_ = folder;
        ImGui::OpenPopup(item_menu_);
    }
    if (!root) {
        item_drag_source(folder);
    }
    if (open) {
        for (uint32_t i = 0; i < entry.child_count; ++i) {
            if (entries_[entry.first_child + i].directory) {
                draw_tree(entry.first_child + i, include_files, selection);
            }
        }
        if (include_files) {
            for (uint32_t i = 0; i < entry.child_count; ++i) {
                const uint32_t file = entry.first_child + i;
                if (entries_[file].directory) {
                    continue;
                }
                const std::string_view file_name = name_of(file);
                char file_label[128];
                static_cast<void>(std::snprintf(file_label, sizeof(file_label), "%s%.*s", kLabelInset,
                                                static_cast<int>(file_name.size()), file_name.data()));
                ImGui::PushID(static_cast<int>(file));
                ImGuiTreeNodeFlags file_flags =
                    ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
                if (is_selected(file)) {
                    file_flags |= ImGuiTreeNodeFlags_Selected;
                }
                static_cast<void>(ImGui::TreeNodeEx("##file", file_flags, "%s", file_label));
                const ImVec2 file_min = ImGui::GetItemRectMin();
                const ImVec2 file_max = ImGui::GetItemRectMax();
                const ImVec2 file_glyph(file_min.x + ImGui::GetTreeNodeToLabelSpacing(), file_min.y);
                leaf_connector(file_min, file_max, file_glyph.x);
                char glyph_name[128];
                static_cast<void>(std::snprintf(glyph_name, sizeof(glyph_name), "%.*s",
                                                static_cast<int>(file_name.size()), file_name.data()));
                inline_document_glyph(file_glyph, file_max.y - file_min.y, glyph_name);
                const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
                if (clicked) {
                    if (entries_[file].parent != current_) {
                        navigate(entries_[file].parent);
                    }
                    rebuild_shown();
                    select(file, SelectMode::Replace, selection);
                }
                if ((clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ||
                    (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
                    open_item(file, selection);
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    context_entry_ = file;
                    ImGui::OpenPopup(item_menu_);
                }
                item_drag_source(file);
                ImGui::PopID();
            }
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

// ---- The folder's contents ----

bool AssetBrowser::view_mode_button(const char* id, ViewMode mode, const char* tip) {
    const bool selected = mode_ == mode;
    const float extent = ImGui::GetFrameHeight();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    const bool pressed = ImGui::InvisibleButton("##mode", ImVec2(extent, extent));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImGui::PopID();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // A segment of the list/grid switch: the chosen one is lifted in the control colour.
    if (selected || hovered || held) {
        const ImGuiCol background = held ? ImGuiCol_ButtonActive : selected ? ImGuiCol_Button : ImGuiCol_ButtonHovered;
        draw->AddRectFilled(min, ImVec2(min.x + extent, min.y + extent), ImGui::GetColorU32(background),
                            ImGui::GetStyle().FrameRounding);
    }
    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
    const float inset = extent * 0.24f;
    const ImVec2 icon_min(min.x + inset, min.y + inset);
    const ImVec2 icon_max(min.x + extent - inset, min.y + extent - inset);
    const float line = std::max(px(1.0f), extent * 0.055f);
    if (mode == ViewMode::List) {
        const float marker = std::max(px(1.5f), extent * 0.11f);
        for (int row = 0; row < 3; ++row) {
            const float y = icon_min.y + ((icon_max.y - icon_min.y) * (static_cast<float>(row) + 0.5f) / 3.0f);
            draw->AddRectFilled(ImVec2(icon_min.x, y - (marker * 0.5f)),
                                ImVec2(icon_min.x + marker, y + (marker * 0.5f)), ink);
            draw->AddLine(ImVec2(icon_min.x + marker + (extent * 0.09f), y), ImVec2(icon_max.x, y), ink, line);
        }
    } else {
        const float gap = std::max(px(1.0f), extent * 0.09f);
        const float cell_width = (icon_max.x - icon_min.x - gap) * 0.5f;
        const float cell_height = (icon_max.y - icon_min.y - gap) * 0.5f;
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 2; ++x) {
                const ImVec2 cell(icon_min.x + (static_cast<float>(x) * (cell_width + gap)),
                                  icon_min.y + (static_cast<float>(y) * (cell_height + gap)));
                draw->AddRect(cell, ImVec2(cell.x + cell_width, cell.y + cell_height), ink, 0.0f, 0, line);
            }
        }
    }
    if (hovered) {
        tooltip("%s", tip);
    }
    return pressed;
}

void AssetBrowser::draw_content_controls() {
    const float start_y = ImGui::GetCursorPosY();
    const float start_x = ImGui::GetCursorPosX();
    const float available = ImGui::GetContentRegionAvail().x;
    const float padding = px(3.0f);
    const float frame = ImGui::GetFrameHeight();
    const float row = frame + (padding * 2.0f);
    ImGui::SetCursorPosY(start_y + padding);
    // Search on the left; the list/grid switch on the right, two segments on one recessed track.
    const float controls = frame * 2.0f;
    const float search_width = std::max(1.0f, std::min(px(420.0f), available - controls - px(24.0f)));
    static_cast<void>(search_field("##search", search_, sizeof(search_), "Search Content", search_width));
    if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        search_[0] = '\0';
    }
    ImGui::SetCursorPos(ImVec2(start_x + std::max(0.0f, available - controls), start_y + padding));
    const ImVec2 track = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(track, ImVec2(track.x + controls, track.y + frame),
                                              ImGui::GetColorU32(ImGuiCol_FrameBg), ImGui::GetStyle().FrameRounding);
    if (view_mode_button("list", ViewMode::List, "List view")) {
        mode_ = ViewMode::List;
        scroll_to_top_ = true;
    }
    ImGui::SameLine(0.0f, 0.0f);
    if (view_mode_button("grid", ViewMode::Grid, "Grid view")) {
        mode_ = ViewMode::Grid;
        scroll_to_top_ = true;
    }
    ImGui::SetCursorPos(ImVec2(start_x, start_y + row));
}

void AssetBrowser::item_interaction(uint32_t entry, bool clicked, bool double_clicked, Selection& selection) {
    if (clicked) {
        select(entry, select_mode(), selection);
    }
    if (double_clicked) {
        open_item(entry, selection);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        if (!is_selected(entry)) {
            select(entry, SelectMode::Replace, selection);
        }
        context_entry_ = entry;
        ImGui::OpenPopup(item_menu_);
    }
}

void AssetBrowser::item_drag_source(uint32_t entry) {
    if (!ImGui::IsItemActive() || !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        return;
    }
    Path path;
    if (path_of(entry, path) && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(kSourceFilePayload, path.c_str(), path.size() + 1);
        const std::string_view name = name_of(entry);
        ImGui::Text("%.*s", static_cast<int>(name.size()), name.data());
        ImGui::EndDragDropSource();
    }
}

void AssetBrowser::item_context_menu(uint32_t entry, Selection& selection) {
    if (entry >= entries_.size()) {
        return;
    }
    Path path;
    const bool have_path = path_of(entry, path);
    const bool directory = entries_[entry].directory;
    const bool world = !directory && file_kind(name_of(entry)) == "World";
    if (ImGui::MenuItem(directory ? "Open Folder" : "Open World", nullptr, false, directory || world)) {
        open_item(entry, selection);
    }
    if (ImGui::MenuItem("Copy Path", nullptr, false, have_path)) {
        ImGui::SetClipboardText(path.c_str());
    }
    if (directory && have_path) {
        bool favorite = false;
        size_t at = SIZE_MAX;
        for (size_t i = 0; i < favorites_.size(); ++i) {
            if (favorites_[i].view() == path.view()) {
                favorite = true;
                at = i;
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(favorite ? "Remove from Favorites" : "Add to Favorites")) {
            if (favorite) {
                favorites_.remove_ordered(at);
            } else {
                favorites_.push(path);
            }
        }
    }
}

void AssetBrowser::keyboard(Selection& selection, uint32_t columns) {
    if (!ImGui::IsWindowFocused() || ImGui::IsAnyItemActive() || shown_.empty()) {
        return;
    }
    const auto* at = anchor_ != UINT32_MAX ? std::ranges::find(shown_, anchor_) : shown_.end();
    const auto position = static_cast<int64_t>(at == shown_.end() ? -1 : at - shown_.begin());
    int64_t step = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        step = columns;
    } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        step = -static_cast<int64_t>(columns);
    } else if (columns > 1 && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        step = 1;
    } else if (columns > 1 && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        step = -1;
    } else if (position >= 0 && ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        open_item(shown_[static_cast<size_t>(position)], selection);
        return;
    }
    if (step == 0) {
        return;
    }
    const int64_t next =
        std::clamp<int64_t>(position < 0 ? 0 : position + step, 0, static_cast<int64_t>(shown_.size()) - 1);
    select(shown_[static_cast<size_t>(next)], ImGui::GetIO().KeyShift ? SelectMode::Range : SelectMode::Replace,
           selection);
    if (ImGui::GetIO().KeyShift) {
        anchor_ = anchor_ == UINT32_MAX ? shown_[static_cast<size_t>(next)] : anchor_;
    }
}

void AssetBrowser::draw_list(Selection& selection) {
    const bool search = search_[0] != '\0';
    const float line = ImGui::GetTextLineHeight();
    // Search rows hold a name and its path: two lines.
    const float row = search ? std::max(screen_px(29.0f), (line * 2.0f) + screen_px(10.0f)) : screen_px(29.0f);
    const float padding = screen_px(6.0f);
    const float icon_slot = screen_px(28.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 1.0f));
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(shown_.size()), row + 1.0f);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const uint32_t entry = shown_[static_cast<size_t>(i)];
            const bool directory = entries_[entry].directory;
            ImGui::PushID(static_cast<int>(entry));
            const bool clicked = ImGui::Selectable("##row", is_selected(entry), ImGuiSelectableFlags_AllowDoubleClick,
                                                   ImVec2(0.0f, row));
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            item_interaction(entry, clicked, clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left), selection);
            item_drag_source(entry);
            char name[128];
            const std::string_view entry_name = name_of(entry);
            static_cast<void>(
                std::snprintf(name, sizeof(name), "%.*s", static_cast<int>(entry_name.size()), entry_name.data()));
            const float center_y = (min.y + max.y) * 0.5f;
            if (directory) {
                const float width = screen_px(20.0f);
                draw_folder_glyph(ImVec2(min.x + padding, center_y - (width * 0.32f)), width, width * 0.64f);
            } else {
                const float extent = screen_px(19.0f);
                draw_document_glyph(ImVec2(min.x + padding, center_y - (extent * 0.5f)), extent, name, false);
            }
            const ImVec2 label_min(min.x + padding + icon_slot, min.y);
            const ImVec2 label_max(max.x - padding, max.y);
            if (search) {
                Path path;
                char where[200] = {};
                if (path_of(entries_[entry].parent, path)) {
                    display_path(path.view(), where, sizeof(where));
                }
                const float top = min.y + ((row - (line * 2.0f) - 2.0f) * 0.5f);
                draw_clipped_label(ImVec2(label_min.x, top), ImVec2(label_max.x, top + line), name,
                                   ImGui::GetColorU32(ImGuiCol_Text), false);
                draw_clipped_label(ImVec2(label_min.x, top + line + 2.0f),
                                   ImVec2(label_max.x, top + (line * 2.0f) + 2.0f), where,
                                   ImGui::GetColorU32(ImGuiCol_TextDisabled), false);
                if (hovered) {
                    tooltip("%s", where);
                }
            } else {
                draw_clipped_label(label_min, label_max, name, ImGui::GetColorU32(ImGuiCol_Text), false);
                if (hovered && ImGui::CalcTextSize(name).x > label_max.x - label_min.x) {
                    tooltip("%s", name);
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::PopStyleVar();
    keyboard(selection, 1);
}

void AssetBrowser::draw_grid(Selection& selection) {
    const bool search = search_[0] != '\0';
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && io.KeyCtrl && io.MouseWheel != 0.0f) {
        grid_extent_ = std::clamp(grid_extent_ + (io.MouseWheel * 8.0f), kGridMin, kGridMax);
    }
    enum class Detail : uint8_t { Compact, Standard, Spacious };
    const Detail detail = grid_extent_ < 72.0f    ? Detail::Compact
                          : grid_extent_ < 120.0f ? Detail::Standard
                                                  : Detail::Spacious;
    const float thumbnail = screen_px(grid_extent_);
    const float padding = screen_px(detail == Detail::Compact ? 5.0f : detail == Detail::Standard ? 7.0f : 9.0f);
    const float spacing = screen_px(detail == Detail::Compact ? 4.0f : detail == Detail::Standard ? 6.0f : 8.0f);
    const bool second_line = search || detail == Detail::Spacious;
    const float caption = ImGui::GetTextLineHeightWithSpacing() * (second_line ? 2.0f : 1.0f);
    const float card_height = thumbnail + (padding * 2.0f) + caption + screen_px(5.0f);
    const float card_width = thumbnail + (padding * 2.0f);
    const float available = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const auto columns = std::max<uint32_t>(1U, static_cast<uint32_t>((available + spacing) / (card_width + spacing)));
    const auto count = static_cast<uint32_t>(shown_.size());
    const uint32_t rows = (count + columns - 1) / columns;
    if (!ImGui::BeginTable("##grid", static_cast<int>(columns), ImGuiTableFlags_SizingStretchSame)) {
        return;
    }
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows), card_height + spacing);
    while (clipper.Step()) {
        for (int visible_row = clipper.DisplayStart; visible_row < clipper.DisplayEnd; ++visible_row) {
            ImGui::TableNextRow(ImGuiTableRowFlags_None, card_height + spacing);
            const uint32_t first = static_cast<uint32_t>(visible_row) * columns;
            const uint32_t last = std::min(count, first + columns);
            for (uint32_t i = first; i < last; ++i) {
                ImGui::TableSetColumnIndex(static_cast<int>(i - first));
                const uint32_t entry = shown_[i];
                const bool directory = entries_[entry].directory;
                ImGui::PushID(static_cast<int>(entry));
                const float width = ImGui::GetContentRegionAvail().x;
                const bool clicked = ImGui::Selectable(
                    "##card", is_selected(entry), ImGuiSelectableFlags_AllowDoubleClick, ImVec2(width, card_height));
                const bool hovered = ImGui::IsItemHovered();
                const ImVec2 min = ImGui::GetItemRectMin();
                const ImVec2 max = ImGui::GetItemRectMax();
                item_interaction(entry, clicked, clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left),
                                 selection);
                item_drag_source(entry);
                char name[128];
                const std::string_view entry_name = name_of(entry);
                static_cast<void>(
                    std::snprintf(name, sizeof(name), "%.*s", static_cast<int>(entry_name.size()), entry_name.data()));
                if (directory) {
                    const float room = std::max(0.0f, max.x - min.x - (padding * 2.0f));
                    const float folder_width = std::min(room, thumbnail * 1.18f);
                    const float folder_height = folder_width * 0.64f;
                    draw_folder_glyph(ImVec2(min.x + ((max.x - min.x - folder_width) * 0.5f),
                                             min.y + padding + std::max(0.0f, (thumbnail - folder_height) * 0.5f)),
                                      folder_width, folder_height);
                } else {
                    const float extent = thumbnail * (detail == Detail::Compact ? 0.78f : 0.84f);
                    draw_document_glyph(ImVec2(min.x + ((max.x - min.x - extent) * 0.5f),
                                               min.y + padding + ((thumbnail - extent) * 0.5f)),
                                        extent, name, detail != Detail::Compact);
                }
                const float caption_top = min.y + padding + thumbnail + screen_px(5.0f);
                const ImVec2 label_min(min.x + padding, caption_top);
                const ImVec2 label_max(max.x - padding, caption_top + ImGui::GetTextLineHeightWithSpacing());
                draw_clipped_label(label_min, label_max, name, ImGui::GetColorU32(ImGuiCol_Text), true);
                char second[200] = {};
                if (search) {
                    Path path;
                    if (path_of(entries_[entry].parent, path)) {
                        display_path(path.view(), second, sizeof(second));
                    }
                } else if (second_line) {
                    const std::string_view kind = directory ? std::string_view("Folder") : file_kind(entry_name);
                    static_cast<void>(
                        std::snprintf(second, sizeof(second), "%.*s", static_cast<int>(kind.size()), kind.data()));
                }
                if (second_line) {
                    draw_clipped_label(ImVec2(label_min.x, label_max.y),
                                       ImVec2(label_max.x, label_max.y + ImGui::GetTextLineHeightWithSpacing()), second,
                                       ImGui::GetColorU32(ImGuiCol_TextDisabled), true);
                }
                if (hovered && search) {
                    tooltip("%s", second);
                } else if (hovered && ImGui::CalcTextSize(name).x > label_max.x - label_min.x) {
                    tooltip("%s", name);
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndTable();
    keyboard(selection, columns);
}

void AssetBrowser::draw_contents(Selection& selection, float region_min_x, float region_max_x) {
    const bool search = search_[0] != '\0';
    const bool one_column = layout_ == Layout::OneColumn;
    const ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
    if (!one_column) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing.x, 0.0f));
        draw_content_controls();
        structural_separator(region_min_x, region_max_x);
        ImGui::PopStyleVar();
    }
    rebuild_shown();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(content_inset(), px(4.0f)));
    const bool visible = ImGui::BeginChild("##contents", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding,
                                           ImGuiWindowFlags_NoNavInputs);
    ImGui::PopStyleVar();
    if (visible) {
        if (scroll_to_top_) {
            ImGui::SetScrollY(0.0f);
            scroll_to_top_ = false;
        }
        if (search && shown_.empty()) {
            ImGui::TextDisabled("No files or folders match your search.");
        } else if (shown_.empty()) {
            ImGui::TextDisabled("This folder is empty.");
        } else if (mode_ == ViewMode::Grid && !one_column) {
            draw_grid(selection);
        } else {
            draw_list(selection);
        }
        if (search && shown_.size() >= kMaxMatches) {
            ImGui::TextDisabled("Showing the first %zu matches. Narrow the search for more.", kMaxMatches);
        }
        // A click on empty space clears the selection.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByPopup) &&
            !ImGui::IsAnyItemHovered()) {
            selected_.clear();
            anchor_ = UINT32_MAX;
        }
    }
    ImGui::EndChild();
}

bool AssetBrowser::draw(const platform::Vfs& vfs, Selection& selection) {
    DE_PROFILE_ZONE("AssetBrowser::draw");
    opened_ = UINT32_MAX;
    if (!ImGui::Begin(kWindow)) {
        ImGui::End();
        return false;
    }
    if (!scanned_ || refresh_) {
        refresh_ = false;
        scan(vfs);
    }
    // Content starts inside the shared inset, but structural lines reach the panel's edges.
    const float frame_min_x = ImGui::GetWindowPos().x;
    const float frame_max_x = frame_min_x + ImGui::GetWindowSize().x;
    const ImVec2 frame_pos = ImGui::GetWindowPos();
    const ImVec2 frame_size = ImGui::GetWindowSize();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool visible = ImGui::BeginChild("##browser", ImVec2(0.0f, 0.0f));
    ImGui::PopStyleVar();
    if (visible) {
        item_menu_ = ImGui::GetID("##item_menu");
        draw_toolbar();
        structural_separator(frame_min_x, frame_max_x);
        const float toolbar_rule_y = ImGui::GetItemRectMin().y;
        if (layout_ == Layout::OneColumn) {
            // One tree with the files in it, or the search's results; on the navigation surface.
            const ImVec2 padding = ImGui::GetStyle().WindowPadding;
            ImGui::PushStyleColor(ImGuiCol_ChildBg, color(ThemeColor::Surface2));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, padding.y));
            const bool shown = ImGui::BeginChild("##tree", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            if (shown) {
                if (search_[0] != '\0') {
                    draw_contents(selection, frame_min_x, frame_max_x);
                } else {
                    ImGui::Indent(padding.x);
                    draw_tree(0, true, selection);
                    ImGui::Unindent(padding.x);
                }
            }
            ImGui::EndChild();
        } else {
            // Start right under the toolbar's rule: item spacing would leave a band above.
            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(ImVec2(cursor.x, toolbar_rule_y + ImGui::GetStyle().SeparatorSize));
            const float cell_padding_x = ImGui::GetStyle().CellPadding.x;
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(cell_padding_x, 0.0f));
            if (ImGui::BeginTable("##layout", 2,
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp |
                                      ImGuiTableFlags_NoPadInnerX)) {
                ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthFixed, px(260.0f));
                ImGui::TableSetupColumn("Contents", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableNextColumn();
                // The folder tree is navigation: a surface step darker than the contents beside it.
                const ImVec2 padding = ImGui::GetStyle().WindowPadding;
                ImGui::PushStyleColor(ImGuiCol_ChildBg, color(ThemeColor::Surface2));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, padding.y));
                const bool tree =
                    ImGui::BeginChild("##folders", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
                if (tree) {
                    ImGui::Indent(padding.x + cell_padding_x);
                    draw_tree(0, false, selection);
                    ImGui::Unindent(padding.x + cell_padding_x);
                }
                ImGui::EndChild();
                ImGui::TableNextColumn();
                // One continuous divider at the columns' border, down to the panel's edge; the table draws none.
                const ImGuiTable* table = ImGui::GetCurrentTable();
                const float divider_x = table->Columns[0].MaxX;
                const bool divider_hovered = table->HoveredColumnBorder == 0;
                const bool divider_active =
                    table->ResizedColumn == 0 && table->InstanceInteracted == table->InstanceCurrent;
                const float bottom = frame_pos.y + frame_size.y;
                if (divider_x > frame_min_x && divider_x < frame_max_x && toolbar_rule_y < bottom) {
                    const ImGuiCol ink = divider_active    ? ImGuiCol_SeparatorActive
                                         : divider_hovered ? ImGuiCol_SeparatorHovered
                                                           : ImGuiCol_TableBorderLight;
                    ImDrawList* draw = ImGui::GetWindowDrawList();
                    draw->PushClipRect(frame_pos, ImVec2(frame_max_x, bottom), true);
                    draw->AddLine(ImVec2(divider_x, toolbar_rule_y), ImVec2(divider_x, bottom), ImGui::GetColorU32(ink),
                                  std::max(1.0f, px(1.0f)));
                    draw->PopClipRect();
                }
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(content_inset() + cell_padding_x, 0.0f));
                const bool contents =
                    ImGui::BeginChild("##folder", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding,
                                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                ImGui::PopStyleVar();
                if (contents) {
                    draw_contents(selection, divider_x, frame_max_x);
                }
                ImGui::EndChild();
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
        }
        // The context menu of the item right-clicked, wherever it was.
        push_menu_style();
        prepare_menu_popup();
        if (ImGui::BeginPopup("##item_menu")) {
            item_context_menu(context_entry_, selection);
            ImGui::EndPopup();
        }
        pop_menu_style();
    }
    ImGui::EndChild();
    ImGui::End();

    const bool search = search_[0] != '\0';
    static_cast<void>(std::snprintf(status_, sizeof(status_), "%zu %s%s  %zu selected", shown_.size(),
                                    search ? "result" : "item", shown_.size() == 1 ? "" : "s", selected_.size()));
    return opened_ != UINT32_MAX;
}

void AssetBrowser::load_settings(const platform::Vfs& vfs) {
    if (!vfs.exists(kSettingsPath)) {
        return;
    }
    auto file = vfs.read_whole_file(kSettingsPath, core::heap_allocator(core::MemoryTag::Editor));
    if (!file) {
        DE_LOG_WARN("editor", "can't read %s: %s", kSettingsPath, file.error().message);
        return;
    }
    std::string_view text(reinterpret_cast<const char*>(file->data()), file->size());
    text = core::skip_bom(text);
    while (!text.empty()) {
        const std::string_view line = core::strip_comment(core::next_line(text));
        const size_t space = line.find(' ');
        const std::string_view key = line.substr(0, space);
        const std::string_view value =
            space == std::string_view::npos ? std::string_view() : core::trim(line.substr(space));
        if (key == "view") {
            mode_ = value == "list" ? ViewMode::List : ViewMode::Grid;
        } else if (key == "layout") {
            layout_ = value == "one" ? Layout::OneColumn : Layout::TwoColumns;
        } else if (key == "thumbnails") {
            char number[32] = {};
            std::memcpy(number, value.data(), std::min(value.size(), sizeof(number) - 1));
            const float extent = std::strtof(number, nullptr);
            grid_extent_ = std::clamp(extent > 0.0f ? extent : 96.0f, kGridMin, kGridMax);
        } else if (key == "favorite" && !value.empty()) {
            Path path;
            if (path.assign(value)) {
                favorites_.push(path);
            }
        }
    }
}

void AssetBrowser::save_settings(const platform::Vfs& vfs) const {
    char text[8192];
    size_t length = 0;
    const auto add = [&](const char* format, auto... values) {
        if (length < sizeof(text)) {
            const int written = std::snprintf(text + length, sizeof(text) - length, format, values...);
            length = written > 0 ? std::min(sizeof(text) - 1, length + static_cast<size_t>(written)) : length;
        }
    };
    add("%s", "# The editor's resource browser: its view, its layout and its favourite folders.\n");
    add("view %s\n", mode_ == ViewMode::List ? "list" : "grid");
    add("layout %s\n", layout_ == Layout::OneColumn ? "one" : "two");
    add("thumbnails %.0f\n", static_cast<double>(grid_extent_));
    for (const Path& favorite : favorites_) {
        add("favorite %s\n", favorite.c_str());
    }
    if (const auto written = vfs.write_file(kSettingsPath, std::as_bytes(std::span(text, length))); !written) {
        DE_LOG_WARN("editor", "can't keep %s: %s", kSettingsPath, written.error().message);
    }
}

} // namespace de::editor
