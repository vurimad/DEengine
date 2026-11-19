#include "editor/properties_panel.h"

#include "app/framework.h"
#include "asset/prefab_format.h"
#include "asset/terrain_format.h"
#include "core/hash.h"
#include "core/profile.h"
#include "editor/commands.h"
#include "editor/outline.h"
#include "editor/source_files.h"
#include "editor/theme.h"
#include "editor/thumbnails.h"
#include "platform/vfs.h"
#include "world/cell.h"
#include "world/prefab.h"
#include "world/render_extract.h"
#include "world/transform.h"
#include "world/world.h"

#include <glm/gtc/quaternion.hpp>
#include <imgui_internal.h> // GetActiveID

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace de::editor {

namespace {

// ---- Names for people ----

bool is_upper(char c) {
    return c >= 'A' && c <= 'Z';
}

bool is_lower(char c) {
    return c >= 'a' && c <= 'z';
}

char upper(char c) {
    return is_lower(c) ? static_cast<char>(c - 'a' + 'A') : c;
}

// The units the engine's field names end in: anything not in meters, seconds, radians or kilograms carries its unit
// in its name, and those often do too.
bool is_unit(std::string_view word) {
    constexpr std::string_view kUnits[] = {"m", "s", "kg", "rad", "deg", "mps", "ms", "ns", "hz", "px", "km", "ticks"};
    return std::ranges::find(kUnits, word) != std::end(kUnits);
}

void append(char* out, size_t capacity, size_t& length, std::string_view text) {
    for (const char c : text) {
        if (length + 1 < capacity) {
            out[length++] = c;
        }
    }
    out[length] = '\0';
}

// A field's name for people: its words capitalized, its unit in brackets. "vertical_fov_rad" -> "Vertical Fov (rad)",
// "half_extents_m" -> "Half Extents (m)", "density_per_m" -> "Density per m".
void field_label(const char* name, char* out, size_t capacity) {
    size_t length = 0;
    out[0] = '\0';
    std::string_view words[16];
    size_t count = 0;
    std::string_view rest(name);
    while (!rest.empty() && count < 16) {
        const size_t underscore = rest.find('_');
        words[count++] = rest.substr(0, underscore);
        rest = underscore == std::string_view::npos ? std::string_view() : rest.substr(underscore + 1);
    }
    const bool unit = count > 1 && is_unit(words[count - 1]) && words[count - 2] != "per";
    const size_t named = unit ? count - 1 : count;
    for (size_t i = 0; i < named; ++i) {
        if (words[i].empty()) {
            continue;
        }
        if (length > 0) {
            append(out, capacity, length, " ");
        }
        const bool small = i > 0 && (words[i] == "per" || words[i] == "of" || words[i - 1] == "per");
        if (small) {
            append(out, capacity, length, words[i]);
        } else {
            const char first = upper(words[i][0]);
            append(out, capacity, length, std::string_view(&first, 1));
            append(out, capacity, length, words[i].substr(1));
        }
    }
    if (unit) {
        append(out, capacity, length, " (");
        append(out, capacity, length, words[count - 1]);
        append(out, capacity, length, ")");
    }
}

// A component's name for people: its words apart. "WorldTransform" -> "World Transform".
void component_label(const char* name, char* out, size_t capacity) {
    size_t length = 0;
    out[0] = '\0';
    for (const char* at = name; *at != '\0'; ++at) {
        if (at != name && is_upper(*at) && is_lower(at[-1])) {
            append(out, capacity, length, " ");
        }
        append(out, capacity, length, std::string_view(at, 1));
    }
}

// ---- Values ----

// The ImGui type a flecs primitive edits as; false for those that aren't numbers.
bool number_type(ecs_primitive_kind_t kind, ImGuiDataType& out) {
    switch (kind) {
    case EcsChar:
    case EcsI8:
        out = ImGuiDataType_S8;
        return true;
    case EcsByte:
    case EcsU8:
        out = ImGuiDataType_U8;
        return true;
    case EcsU16:
        out = ImGuiDataType_U16;
        return true;
    case EcsI16:
        out = ImGuiDataType_S16;
        return true;
    case EcsU32:
        out = ImGuiDataType_U32;
        return true;
    case EcsI32:
        out = ImGuiDataType_S32;
        return true;
    case EcsU64:
    case EcsUPtr:
        out = ImGuiDataType_U64;
        return true;
    case EcsI64:
    case EcsIPtr:
        out = ImGuiDataType_S64;
        return true;
    case EcsF32:
        out = ImGuiDataType_Float;
        return true;
    case EcsF64:
        out = ImGuiDataType_Double;
        return true;
    default:
        return false;
    }
}

bool is_real(ImGuiDataType type) {
    return type == ImGuiDataType_Float || type == ImGuiDataType_Double;
}

double read_number(ImGuiDataType type, const std::byte* data);

// The format that shows a number whole in a field width wide: up to millimetres (three decimals), without trailing
// zeros, and fewer decimals while it doesn't fit, so the part before the point always shows ("64", "1.2", "0.19").
// Integers as ImGui shows them. The field's tooltip has the whole value.
const char* fitting_format(ImGuiDataType type, const std::byte* data, float width, char (&format)[8]) {
    if (!is_real(type)) {
        return nullptr;
    }
    const double value = read_number(type, data);
    const float room = width - (ImGui::GetStyle().FramePadding.x * 2.0f);
    char text[64];
    static_cast<void>(std::snprintf(text, sizeof(text), "%.3f", value));
    int decimals = 3;
    for (size_t length = std::strlen(text); decimals > 0 && text[length - 1] == '0'; --length) {
        --decimals;
    }
    for (; decimals > 0; --decimals) {
        static_cast<void>(std::snprintf(text, sizeof(text), "%.*f", decimals, value));
        if (ImGui::CalcTextSize(text).x <= room) {
            break;
        }
    }
    static_cast<void>(std::snprintf(format, sizeof(format), "%%.%df", decimals));
    return format;
}

float drag_speed(ImGuiDataType type) {
    return type == ImGuiDataType_Double ? 0.05f : type == ImGuiDataType_Float ? 0.01f : 0.2f;
}

double read_number(ImGuiDataType type, const std::byte* data) {
    const auto read = [data]<typename T>(T value) {
        std::memcpy(&value, data, sizeof(T));
        return static_cast<double>(value);
    };
    switch (type) {
    case ImGuiDataType_S8:
        return read(int8_t{});
    case ImGuiDataType_U8:
        return read(uint8_t{});
    case ImGuiDataType_S16:
        return read(int16_t{});
    case ImGuiDataType_U16:
        return read(uint16_t{});
    case ImGuiDataType_S32:
        return read(int32_t{});
    case ImGuiDataType_U32:
        return read(uint32_t{});
    case ImGuiDataType_S64:
        return read(int64_t{});
    case ImGuiDataType_U64:
        return read(uint64_t{});
    case ImGuiDataType_Float:
        return read(float{});
    default:
        return read(double{});
    }
}

int64_t read_integer(ecs_primitive_kind_t kind, const std::byte* data) {
    ImGuiDataType type = ImGuiDataType_S32;
    return number_type(kind, type) ? static_cast<int64_t>(read_number(type, data)) : 0;
}

void write_integer(ecs_primitive_kind_t kind, std::byte* data, int64_t value) {
    ImGuiDataType type = ImGuiDataType_S32;
    if (!number_type(kind, type) || is_real(type)) {
        return;
    }
    const auto write = [data]<typename T>(T narrowed) { std::memcpy(data, &narrowed, sizeof(T)); };
    switch (type) {
    case ImGuiDataType_S8:
        write(static_cast<int8_t>(value));
        break;
    case ImGuiDataType_U8:
        write(static_cast<uint8_t>(value));
        break;
    case ImGuiDataType_S16:
        write(static_cast<int16_t>(value));
        break;
    case ImGuiDataType_U16:
        write(static_cast<uint16_t>(value));
        break;
    case ImGuiDataType_S32:
        write(static_cast<int32_t>(value));
        break;
    case ImGuiDataType_U32:
        write(static_cast<uint32_t>(value));
        break;
    default:
        write(value);
        break;
    }
}

const EcsPrimitive* primitive_of(const flecs::world& ecs, flecs::entity_t type) {
    return ecs_get(ecs.c_ptr(), type, EcsPrimitive);
}

// A struct of 2 to 4 numbers of one kind, back to back (a vector, a quaternion): edited on one row.
struct InlineNumbers {
    bool ok = false;
    ImGuiDataType type = ImGuiDataType_Float;
    int count = 0;
    size_t stride = 0;
};

InlineNumbers inline_numbers(const flecs::world& ecs, flecs::entity_t type) {
    const EcsStruct* structure = ecs_get(ecs.c_ptr(), type, EcsStruct);
    if (structure == nullptr) {
        return {};
    }
    const auto* members = ecs_vec_first_t(&structure->members, ecs_member_t);
    const int32_t count = ecs_vec_count(&structure->members);
    if (count < 2 || count > 4) {
        return {};
    }
    const EcsPrimitive* first = primitive_of(ecs, members[0].type);
    InlineNumbers out;
    if (first == nullptr || !number_type(first->kind, out.type)) {
        return {};
    }
    const ecs_type_info_t* info = ecs_get_type_info(ecs.c_ptr(), members[0].type);
    out.stride = info != nullptr ? static_cast<size_t>(info->size) : 0;
    for (int32_t i = 0; i < count; ++i) {
        const EcsPrimitive* each = primitive_of(ecs, members[i].type);
        if (each == nullptr || each->kind != first->kind || members[i].count > 1 ||
            static_cast<size_t>(members[i].offset) != static_cast<size_t>(i) * out.stride) {
            return {};
        }
    }
    out.ok = out.stride > 0;
    out.count = count;
    return out;
}

// A value as text, for Copy: numbers, booleans, and small structs of them.
void value_text(const flecs::world& ecs, flecs::entity_t type, const std::byte* data, int32_t count, char* out,
                size_t capacity) {
    out[0] = '\0';
    size_t length = 0;
    const EcsPrimitive* primitive = primitive_of(ecs, type);
    ImGuiDataType number = ImGuiDataType_S32;
    const InlineNumbers inline_struct = inline_numbers(ecs, type);
    const ecs_type_info_t* info = ecs_get_type_info(ecs.c_ptr(), type);
    const size_t element = info != nullptr ? static_cast<size_t>(info->size) : 0;
    for (int32_t i = 0; i < std::max(count, 1); ++i) {
        const std::byte* at = data + (static_cast<size_t>(i) * element);
        char part[96] = {};
        if (primitive != nullptr && number_type(primitive->kind, number)) {
            static_cast<void>(std::snprintf(part, sizeof(part), "%.9g", read_number(number, at)));
        } else if (primitive != nullptr && primitive->kind == EcsBool) {
            static_cast<void>(std::snprintf(part, sizeof(part), "%s", at[0] != std::byte{0} ? "true" : "false"));
        } else if (inline_struct.ok) {
            size_t part_length = 0;
            for (int j = 0; j < inline_struct.count; ++j) {
                char component[32];
                static_cast<void>(std::snprintf(
                    component, sizeof(component), "%s%.9g", j > 0 ? ", " : "",
                    read_number(inline_struct.type, at + (static_cast<size_t>(j) * inline_struct.stride))));
                append(part, sizeof(part), part_length, component);
            }
        }
        if (i > 0) {
            append(out, capacity, length, ", ");
        }
        append(out, capacity, length, part);
    }
}

// Whether the editor may change the component: plain data the engine doesn't compute or own. What it computes
// (world transforms), what holds or names assets (a cell, a mesh), and handles into other systems are greyed out.
bool editable(const flecs::world& ecs, flecs::id_t component) {
    const ecs_type_info_t* info = ecs_get_type_info(ecs.c_ptr(), component);
    if (info == nullptr || info->hooks.copy != nullptr || info->hooks.move != nullptr) {
        return false; // not plain data: copying its bytes would break what it holds
    }
    if (ecs_get(ecs.c_ptr(), component, EcsType) == nullptr) {
        return false; // no reflection: nothing to show of it
    }
    const flecs::id_t computed[] = {
        ecs.component<world::WorldTransform>().id(), ecs.component<world::PreviousWorldTransform>().id(),
        ecs.component<world::CellContents>().id(),   ecs.component<world::CellMember>().id(),
        ecs.component<world::RenderSlot>().id(),     ecs.component<world::ActiveCamera>().id(),
        ecs.component<world::MeshInstance>().id(), // asset handles: what spawned it holds the asset
        ecs.component<world::MeshCollider>().id(),
    };
    return std::ranges::find(computed, component) == std::end(computed);
}

// A key for one widget's edits of one component on one entity: a drag's sets make one command.
uint64_t merge_key(flecs::entity_t entity, flecs::id_t component, uint32_t item) {
    return core::mix64(entity ^ core::mix64(component ^ (uint64_t{item} << 32U))) | 1U;
}

// One drag field per number on the row, each with its axis' tint on its left edge. True if one changed.
bool inline_fields(ImGuiDataType type, std::byte* data, int count, size_t stride, float width, uint32_t& item,
                   bool& ended) {
    constexpr ThemeColor kAxes[] = {ThemeColor::AxisX, ThemeColor::AxisY, ThemeColor::AxisZ, ThemeColor::TextDisabled};
    constexpr const char* kAxisNames[] = {"X", "Y", "Z", "W"};
    const float gap = px(3.0f);
    const float field = std::floor((width - (gap * static_cast<float>(count - 1))) / static_cast<float>(count));
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        ImGui::PushID(i);
        if (i > 0) {
            ImGui::SameLine(0.0f, gap);
        }
        const ImVec2 min = ImGui::GetCursorScreenPos();
        std::byte* value = data + (static_cast<size_t>(i) * stride);
        char format[8];
        ImGui::SetNextItemWidth(std::max(1.0f, field));
        if (ImGui::DragScalar("##component", type, value, drag_speed(type), nullptr, nullptr,
                              fitting_format(type, value, field, format))) {
            item = ImGui::GetItemID();
            changed = true;
        }
        ended = ended || ImGui::IsItemDeactivatedAfterEdit();
        if (is_real(type) && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive()) {
            tooltip("%s %.6g", kAxisNames[std::min(i, 3)], read_number(type, value));
        }
        ImGui::GetWindowDrawList()->AddRectFilled(min, ImVec2(min.x + px(2.0f), min.y + ImGui::GetFrameHeight()),
                                                  color_u32(kAxes[std::min(i, 3)]));
        ImGui::PopID();
    }
    return changed;
}

// Whether one of the fields inline_fields() drew in the current ID scope is being dragged or typed in.
bool inline_fields_active(int count) {
    bool active = false;
    for (int i = 0; i < count; ++i) {
        ImGui::PushID(i);
        active = active || (ImGui::GetActiveID() != 0 && ImGui::GetActiveID() == ImGui::GetID("##component"));
        ImGui::PopID();
    }
    return active;
}

// Text in the value column, centred on the row's frame line.
void value_label(const PropertyRow& row, const char* text, ThemeColor ink) {
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    draw_clipped_text(ImGui::GetWindowDrawList(),
                      ImVec2(cursor.x + ImGui::GetStyle().FramePadding.x,
                             std::floor(cursor.y + ((ImGui::GetFrameHeight() - ImGui::GetFontSize()) * 0.5f))),
                      row.value_end, text, color_u32(ink));
}

// The rows' label/value divider: it drags anywhere along the rows. Drawn before the rows, so it takes the mouse over
// their widgets.
void draw_divider(float rows_min_x, float rows_width, float top, float bottom, float& label_fraction) {
    if (rows_width <= 0.0f) {
        return;
    }
    const float divider_x = rows_min_x + property_label_width(rows_width, label_fraction);
    const float grip = px(3.0f);
    ImGui::SetCursorScreenPos(ImVec2(divider_x - grip, top));
    static_cast<void>(ImGui::InvisibleButton("##divider", ImVec2(grip * 2.0f, std::max(1.0f, bottom - top))));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (ImGui::IsItemActive()) {
        label_fraction = std::clamp((ImGui::GetIO().MousePos.x - rows_min_x) / rows_width, 0.15f, 0.8f);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(divider_x - 1.0f, top), ImVec2(divider_x + 1.0f, bottom),
                                                  color_u32(ThemeColor::Accent));
    }
}

// The centred message of an empty panel.
void empty_message(const char* text) {
    const ImVec2 size = ImGui::CalcTextSize(text);
    ImGui::SetCursorPos(ImVec2(std::max(px(8.0f), (ImGui::GetWindowWidth() - size.x) * 0.5f),
                               ImGui::GetCursorStartPos().y + px(16.0f)));
    ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - px(8.0f));
    ImGui::TextColored(color(ThemeColor::TextSecondary), "%s", text);
    ImGui::PopTextWrapPos();
}

} // namespace

bool PropertiesPanel::matches(const flecs::world& ecs, const char* name, flecs::entity_t type) const {
    const std::string_view filter(filter_);
    if (filter.empty()) {
        return true;
    }
    char label[96];
    field_label(name, label, sizeof(label));
    if (contains_ignoring_case(label, filter) || contains_ignoring_case(name, filter)) {
        return true;
    }
    if (const EcsStruct* structure = ecs_get(ecs.c_ptr(), type, EcsStruct)) {
        const auto* members = ecs_vec_first_t(&structure->members, ecs_member_t);
        for (int32_t i = 0; i < ecs_vec_count(&structure->members); ++i) {
            if (matches(ecs, members[i].name, members[i].type)) {
                return true;
            }
        }
    }
    return false;
}

void PropertiesPanel::row_extras(const PropertyRow& row, const char* label, const char* name, const char* type_name,
                                 const char* value_text_copy, bool can_reset, bool& reset) {
    // The label's tooltip after the hover delay: its name for people, its field name and type.
    const ImGuiID id = ImGui::GetID("##label");
    if (property_label_hovered(row)) {
        if (hovered_label_ != id) {
            hovered_label_ = id;
            hover_start_s_ = ImGui::GetTime();
        } else if (ImGui::GetTime() - hover_start_s_ >= static_cast<double>(ImGui::GetStyle().HoverDelayNormal)) {
            tooltip("%s\n%s (%s)", label, name, type_name);
        }
    } else if (hovered_label_ == id) {
        hovered_label_ = 0;
    }
    // The row's context menu.
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
        ImGui::IsMouseHoveringRect(row.min, ImVec2(row.min.x + row.width, row.min.y + row.height))) {
        ImGui::OpenPopup("##property_menu");
    }
    push_menu_style();
    if (ImGui::BeginPopup("##property_menu")) {
        if (ImGui::MenuItem("Copy", nullptr, false, value_text_copy != nullptr && value_text_copy[0] != '\0')) {
            ImGui::SetClipboardText(value_text_copy);
        }
        if (ImGui::MenuItem("Copy Name")) {
            ImGui::SetClipboardText(name);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Revert to Saved", nullptr, false, can_reset)) {
            reset = true;
        }
        ImGui::EndPopup();
    }
    pop_menu_style();
}

void PropertiesPanel::draw_field(const flecs::world& ecs, const char* name, flecs::entity_t type, int32_t count,
                                 const Value& value, size_t offset, uint32_t depth, Edit& edit) {
    if (!matches(ecs, name, type)) {
        return;
    }
    flecs::world_t* world = ecs.c_ptr();
    const ecs_type_info_t* info = ecs_get_type_info(world, type);
    const size_t element = info != nullptr ? static_cast<size_t>(info->size) : 0;
    const int32_t elements = std::max(count, 1);
    const size_t bytes = element * static_cast<size_t>(elements);
    std::byte* data = value.data + offset;
    const bool differs =
        value.saved != nullptr && bytes > 0 && !value.read_only && std::memcmp(data, value.saved + offset, bytes) != 0;

    const EcsPrimitive* primitive = primitive_of(ecs, type);
    const EcsEnum* enumeration = ecs_get(world, type, EcsEnum);
    const EcsStruct* structure = ecs_get(world, type, EcsStruct);
    ImGuiDataType number = ImGuiDataType_S32;
    const bool numeric = primitive != nullptr && number_type(primitive->kind, number);
    const InlineNumbers inline_struct = elements == 1 ? inline_numbers(ecs, type) : InlineNumbers{};
    const bool inline_array = numeric && elements >= 2 && elements <= 4;
    const bool expandable = (structure != nullptr && !inline_struct.ok) || (elements > 1 && !inline_array);

    char label[96];
    if (name[0] == '[') {
        static_cast<void>(std::snprintf(label, sizeof(label), "%s", name));
    } else {
        field_label(name, label, sizeof(label));
    }
    ImGui::PushID(name);
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID open_id = ImGui::GetID("##open");
    bool expanded = storage->GetBool(open_id, false);
    const PropertyRow row =
        begin_property_row(label, depth, expandable ? &expanded : nullptr,
                           value.read_only ? ThemeColor::TextSecondary : ThemeColor::Text, label_fraction_);
    storage->SetBool(open_id, expanded);
    const float width = property_value_width(row);
    const auto note = [&edit](bool changed) {
        if (changed) {
            edit.item = ImGui::GetItemID();
        }
        edit.ended = edit.ended || ImGui::IsItemDeactivatedAfterEdit();
        return changed;
    };
    ImGui::BeginDisabled(value.read_only);
    if (inline_array) {
        static_cast<void>(inline_fields(number, data, elements, element, width, edit.item, edit.ended));
    } else if (inline_struct.ok) {
        static_cast<void>(inline_fields(inline_struct.type, data, inline_struct.count, inline_struct.stride, width,
                                        edit.item, edit.ended));
    } else if (numeric && !expandable) {
        char format[8];
        ImGui::SetNextItemWidth(width);
        static_cast<void>(note(ImGui::DragScalar("##value", number, data, drag_speed(number), nullptr, nullptr,
                                                 fitting_format(number, data, width, format))));
        if (is_real(number) && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive()) {
            tooltip("%.9g", read_number(number, data));
        }
    } else if (primitive != nullptr && primitive->kind == EcsBool && !expandable) {
        bool checked = data[0] != std::byte{0};
        if (note(compact_checkbox(checked))) {
            data[0] = checked ? std::byte{1} : std::byte{0};
            edit.ended = true; // a click is one edit
        }
    } else if (primitive != nullptr && primitive->kind == EcsEntity && !expandable) {
        flecs::entity_t target = 0;
        std::memcpy(&target, data, sizeof(target));
        const char* target_name = target != 0 && ecs.is_alive(target) ? ecs_get_name(world, target) : nullptr;
        char text[64];
        if (target_name != nullptr) {
            static_cast<void>(std::snprintf(text, sizeof(text), "%s", target_name));
        } else if (target != 0) {
            static_cast<void>(std::snprintf(text, sizeof(text), "#%" PRIu64, static_cast<uint64_t>(target)));
        } else {
            static_cast<void>(std::snprintf(text, sizeof(text), "None"));
        }
        value_label(row, text, ThemeColor::Text);
    } else if (enumeration != nullptr && !expandable) {
        // Its constants are its children, each with its value in (Constant, underlying type).
        const EcsPrimitive* underlying = primitive_of(ecs, enumeration->underlying_type);
        const flecs::id_t constant_id = ecs_pair(EcsConstant, enumeration->underlying_type);
        const int64_t current = underlying != nullptr ? read_integer(underlying->kind, data) : 0;
        const auto each_constant = [&](auto&& visit) {
            ecs.entity(type).children([&](flecs::entity constant) {
                const void* constant_value = ecs_get_id(world, constant.id(), constant_id);
                if (constant_value != nullptr && underlying != nullptr) {
                    visit(constant, read_integer(underlying->kind, static_cast<const std::byte*>(constant_value)));
                }
            });
        };
        const char* preview = "?";
        each_constant([&](flecs::entity constant, int64_t constant_number) {
            if (constant_number == current) {
                preview = constant.name().c_str();
            }
        });
        if (begin_combo("##value", preview, width)) {
            each_constant([&](flecs::entity constant, int64_t constant_number) {
                if (ImGui::Selectable(constant.name().c_str(), constant_number == current) &&
                    constant_number != current && underlying != nullptr) {
                    write_integer(underlying->kind, data, constant_number);
                    edit.item = ImGui::GetItemID();
                    edit.ended = true; // a choice is one edit
                }
            });
            end_combo();
        }
    } else {
        // A struct or a list: what it is; its fields open under it.
        char text[64];
        if (elements > 1) {
            static_cast<void>(std::snprintf(text, sizeof(text), "%d elements", elements));
        } else {
            const char* type_name = ecs_get_name(world, type);
            component_label(type_name != nullptr ? type_name : "?", text, sizeof(text));
        }
        value_label(row, text, ThemeColor::TextSecondary);
    }
    ImGui::EndDisabled();

    bool reset = property_reset_button(row, differs);
    char copy[256];
    value_text(ecs, type, data, elements, copy, sizeof(copy));
    const char* type_name = ecs_get_name(world, type);
    row_extras(row, label, name, type_name != nullptr ? type_name : "?", copy, differs, reset);
    if (reset && differs) {
        std::memcpy(data, value.saved + offset, bytes);
        edit.reset = true;
    }
    end_property_row(row);

    if (expandable && (expanded || filter_[0] != '\0')) {
        if (structure != nullptr && elements == 1) {
            draw_members(ecs, type, value, offset, depth + 1, edit);
        } else {
            for (int32_t i = 0; i < elements; ++i) {
                char index[16];
                static_cast<void>(std::snprintf(index, sizeof(index), "[%d]", i));
                draw_field(ecs, index, type, 1, value, offset + (static_cast<size_t>(i) * element), depth + 1, edit);
            }
        }
    }
    ImGui::PopID();
}

void PropertiesPanel::draw_members(const flecs::world& ecs, flecs::entity_t type, const Value& value, size_t offset,
                                   uint32_t depth, Edit& edit) {
    const EcsStruct* structure = ecs_get(ecs.c_ptr(), type, EcsStruct);
    if (structure == nullptr) {
        return;
    }
    const auto* members = ecs_vec_first_t(&structure->members, ecs_member_t);
    for (int32_t i = 0; i < ecs_vec_count(&structure->members); ++i) {
        draw_field(ecs, members[i].name, members[i].type, members[i].count, value,
                   offset + static_cast<size_t>(members[i].offset), depth, edit);
    }
}

void PropertiesPanel::draw_transform(const Value& value, flecs::entity_t entity, Edit& edit) {
    world::Transform transform;
    std::memcpy(&transform, value.data, sizeof(transform));
    // What it was before this session's edits: the reset arrows' target.
    world::Transform saved;
    if (value.saved != nullptr) {
        std::memcpy(&saved, value.saved, sizeof(saved));
    }
    const bool has_saved = value.saved != nullptr;
    const auto row_of = [&](const char* label, const char* name, bool differs, auto&& value_widget, auto&& revert) {
        if (filter_[0] != '\0' && !contains_ignoring_case(label, filter_)) {
            return;
        }
        ImGui::PushID(name);
        const PropertyRow row = begin_property_row(label, 0, nullptr, ThemeColor::Text, label_fraction_);
        value_widget(property_value_width(row));
        bool reset = property_reset_button(row, differs);
        row_extras(row, label, name,
                   std::string_view(name) == "position"   ? "meters, in the parent's space"
                   : std::string_view(name) == "rotation" ? "degrees about X, Y and Z"
                                                          : "times its size",
                   "", differs, reset);
        if (reset && differs) {
            revert();
            edit.reset = true;
        }
        end_property_row(row);
        ImGui::PopID();
    };
    row_of(
        "Position", "position", has_saved && transform.position != saved.position,
        [&](float width) {
            static_cast<void>(inline_fields(ImGuiDataType_Double, reinterpret_cast<std::byte*>(&transform.position.x),
                                            3, sizeof(double), width, edit.item, edit.ended));
        },
        [&] { transform.position = saved.position; });
    // Angles about X, Y and Z, in degrees; while dragged, the ones being dragged.
    Vec3 angles = rotation_active_ && rotation_entity_ == entity ? rotation_deg_
                                                                 : glm::degrees(glm::eulerAngles(transform.rotation));
    for (int i = 0; i < 3; ++i) {
        // Read back from a rotation, 0 comes out as -0.000001: shown as "-0".
        angles[i] = std::abs(angles[i]) < 0.0005f ? 0.0f : angles[i];
    }
    bool rotating = false;
    row_of(
        "Rotation", "rotation", has_saved && transform.rotation != saved.rotation,
        [&](float width) {
            if (inline_fields(ImGuiDataType_Float, reinterpret_cast<std::byte*>(&angles.x), 3, sizeof(float), width,
                              edit.item, edit.ended)) {
                transform.rotation = glm::normalize(Quat(glm::radians(angles)));
            }
            rotating = inline_fields_active(3);
        },
        [&] {
            transform.rotation = saved.rotation;
            angles = glm::degrees(glm::eulerAngles(saved.rotation));
        });
    rotation_active_ = rotating;
    rotation_entity_ = entity;
    rotation_deg_ = angles;
    row_of(
        "Scale", "scale", has_saved && transform.scale != saved.scale,
        [&](float width) {
            static_cast<void>(inline_fields(ImGuiDataType_Float, reinterpret_cast<std::byte*>(&transform.scale.x), 3,
                                            sizeof(float), width, edit.item, edit.ended));
        },
        [&] { transform.scale = saved.scale; });
    std::memcpy(value.data, &transform, sizeof(transform));
}

void PropertiesPanel::draw_entity(const app::Engine& engine, Selection& selection, CommandStack& commands) {
    const flecs::world& ecs = engine.world.ecs();
    flecs::world_t* world = ecs.c_ptr();
    if (!ecs.is_alive(selection.entity)) {
        empty_message("The selected entity is gone.");
        return;
    }
    const flecs::entity entity = ecs.entity(selection.entity);

    // Search, then the entity: its name, then what it is and where it is.
    ImGui::SetCursorPos(ImVec2(px(6.0f), ImGui::GetCursorStartPos().y + px(4.0f)));
    static_cast<void>(search_field("##search", filter_, sizeof(filter_), "Search properties",
                                   ImGui::GetContentRegionAvail().x - px(6.0f)));
    char label[128];
    entity_label(engine.assets, entity, label);
    ImGui::SetCursorPosX(px(10.0f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(3.0f));
    if (fonts().semibold != nullptr) {
        ImGui::PushFont(fonts().semibold, 0.0f);
    }
    ImGui::TextColored(color(ThemeColor::TextEmphasis), "%s", label);
    if (fonts().semibold != nullptr) {
        ImGui::PopFont();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        tooltip("Entity %u", static_cast<uint32_t>(entity.id()));
    }
    ImGui::SetCursorPosX(px(10.0f));
    ImGui::TextColored(color(ThemeColor::TextSecondary), "%s", entity_kind(entity));
    const auto link = [&](const char* what, flecs::entity target) {
        char name[128];
        entity_label(engine.assets, target, name);
        ImGui::SameLine(0.0f, px(10.0f));
        ImGui::TextColored(color(ThemeColor::TextSecondary), "%s", what);
        ImGui::SameLine(0.0f, px(4.0f));
        ImGui::PushID(static_cast<int>(static_cast<uint32_t>(target.id())));
        if (ImGui::TextLink(name)) {
            selection.select_entity(target.id());
        }
        ImGui::PopID();
    };
    if (const flecs::entity cell = entity.target<world::InCell>(); cell.is_valid()) {
        link("in", cell);
    }
    if (const auto* parent = entity.try_get<flecs::Parent>(); parent != nullptr && ecs.is_alive(parent->value)) {
        link("attached to", ecs.entity(parent->value));
    } else if (const flecs::entity child_of = entity.parent(); child_of.is_valid()) {
        link("attached to", child_of);
    }
    ImGui::Dummy(ImVec2(0.0f, px(2.0f)));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::BeginChild("##rows", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        const ImVec2 rows_min = ImGui::GetCursorScreenPos();
        draw_divider(rows_min.x, ImGui::GetContentRegionAvail().x, ImGui::GetWindowPos().y,
                     ImGui::GetWindowPos().y + ImGui::GetWindowHeight(), label_fraction_);
        ImGui::SetCursorScreenPos(rows_min);

        const flecs::id_t transform_id = ecs.component<world::Transform>().id();
        const flecs::id_t parent_id = ecs_id(EcsParent);
        char tags[512] = {};
        size_t tags_length = 0;
        uint32_t engine_hidden = 0; // what the engine computes or owns, while it's hidden
        const auto draw_component = [&](flecs::id_t component) {
            const char* name = ecs_get_name(world, component);
            const ecs_type_info_t* info = ecs_get_type_info(world, component);
            if (info == nullptr || info->size <= 0) {
                // A tag: listed together at the end, with the engine's data.
                char tag[64];
                component_label(name != nullptr ? name : "?", tag, sizeof(tag));
                append(tags, sizeof(tags), tags_length, tags_length > 0 ? ", " : "");
                append(tags, sizeof(tags), tags_length, tag);
                return;
            }
            const bool can_edit = editable(ecs, component);
            if (!can_edit && !show_engine_) {
                ++engine_hidden;
                return;
            }
            char category[96];
            component_label(name != nullptr ? name : "?", category, sizeof(category));
            ImGui::PushID(static_cast<int>(static_cast<uint32_t>(component)));
            if (begin_category(category, can_edit) || filter_[0] != '\0') {
                const void* current = ecs_get_id(world, entity.id(), component);
                scratch_.resize(static_cast<size_t>(info->size));
                std::memcpy(scratch_.data(), current, scratch_.size());
                Edit edit;
                const Value value{.data = scratch_.data(),
                                  .saved = can_edit ? commands.saved_value(entity.id(), component) : nullptr,
                                  .read_only = !can_edit};
                if (component == transform_id) {
                    draw_transform(value, entity.id(), edit);
                } else if (ecs_get(world, component, EcsType) == nullptr) {
                    // Registered without its members: there's nothing to show.
                    const PropertyRow row =
                        begin_property_row("Fields", 0, nullptr, ThemeColor::TextSecondary, label_fraction_);
                    value_label(row, "Not reflected", ThemeColor::TextSecondary);
                    end_property_row(row);
                } else if (ecs_get(world, component, EcsStruct) != nullptr) {
                    draw_members(ecs, component, value, 0, 0, edit);
                } else {
                    draw_field(ecs, name != nullptr ? name : "value", component, 1, value, 0, 0, edit);
                }
                if (can_edit && edit.reset) {
                    commands.seal(); // a revert is a command of its own
                    static_cast<void>(commands.set_component(entity, component, scratch_.span()));
                    commands.seal();
                } else if (can_edit && std::memcmp(scratch_.data(), current, scratch_.size()) != 0) {
                    static_cast<void>(commands.set_component(entity, component, scratch_.span(),
                                                             merge_key(entity.id(), component, edit.item)));
                }
                if (edit.ended) {
                    commands.seal();
                }
            }
            ImGui::PopID();
        };
        // What it's made from, the Transform, then the rest in the entity's order.
        draw_made_from(engine, entity, commands);
        if (entity.has(transform_id)) {
            draw_component(transform_id);
        }
        entity.each([&](flecs::id id) {
            if (id.is_pair() || id == parent_id || id == transform_id) {
                return; // its name, its cell and its parent are above
            }
            draw_component(id.raw_id());
        });
        if (show_engine_ && tags_length > 0 && filter_[0] == '\0' && begin_category("Tags", true)) {
            ImGui::Dummy(ImVec2(0.0f, px(3.0f)));
            ImGui::SetCursorPosX(px(20.0f));
            ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - px(12.0f));
            ImGui::TextColored(color(ThemeColor::TextSecondary), "%s", tags);
            ImGui::PopTextWrapPos();
        }
        engine_hidden += !show_engine_ && tags_length > 0 ? 1 : 0;
        // What the engine computes and owns stays out of the way until asked for.
        if (engine_hidden > 0 || show_engine_) {
            ImGui::Dummy(ImVec2(0.0f, px(8.0f)));
            ImGui::SetCursorPosX(px(12.0f));
            char text[64];
            if (show_engine_) {
                static_cast<void>(std::snprintf(text, sizeof(text), "Hide the engine's data"));
            } else {
                static_cast<void>(std::snprintf(text, sizeof(text), "Show the engine's data (%u)", engine_hidden));
            }
            ImGui::PushStyleColor(ImGuiCol_TextLink, color(ThemeColor::TextSecondary));
            if (ImGui::TextLink(text)) {
                show_engine_ = !show_engine_;
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                tooltip("What the engine computes or owns: world transforms, render slots, the cell's bookkeeping. "
                        "Read only.");
            }
            ImGui::Dummy(ImVec2(0.0f, px(8.0f)));
        }
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void PropertiesPanel::draw_made_from(const app::Engine& engine, flecs::entity entity, CommandStack& commands) {
    if (sources_.entity != entity.id() || filter_[0] != '\0') {
        return;
    }
    const auto ask = [&](PropertiesRequest::Kind kind, std::string_view path) {
        request_.kind = kind;
        static_cast<void>(request_.path.assign(path));
    };
    // A source: its picture if it has one, its file (a link that shows it in Resources) and the folder it's in.
    const auto source = [&](std::string_view path) {
        const size_t slash = path.rfind('/');
        const std::string_view file = slash == std::string_view::npos ? path : path.substr(slash + 1);
        std::string_view folder = slash == std::string_view::npos ? std::string_view() : path.substr(0, slash);
        if (folder.starts_with("src:/")) {
            folder.remove_prefix(5);
        }
        const float extent = px(44.0f);
        const float inset = px(20.0f);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        float text_x = at.x + inset;
        Thumbnails::Picture picture;
        if (thumbnails_ != nullptr && thumbnails_->find(path, picture)) {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 min(at.x + inset, at.y + px(4.0f));
            draw->AddRectFilled(min, ImVec2(min.x + extent, min.y + extent), color_u32(ThemeColor::Control),
                                px(theme().metrics.control_rounding));
            draw->AddImage(picture.texture, min, ImVec2(min.x + extent, min.y + extent), picture.uv_min,
                           picture.uv_max);
            text_x = min.x + extent + px(10.0f);
        }
        char name[160];
        static_cast<void>(std::snprintf(name, sizeof(name), "%.*s", static_cast<int>(file.size()), file.data()));
        ImGui::SetCursorScreenPos(ImVec2(text_x, at.y + px(8.0f)));
        ImGui::PushID(name);
        if (ImGui::TextLink(name)) {
            ask(PropertiesRequest::Kind::ShowFile, path);
        }
        ImGui::PopID();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            tooltip("%.*s\nShow it in Resources", static_cast<int>(path.size()), path.data());
        }
        ImGui::SetCursorScreenPos(ImVec2(text_x, at.y + px(8.0f) + ImGui::GetFontSize() + px(4.0f)));
        ImGui::TextColored(color(ThemeColor::TextSecondary), "%.*s", static_cast<int>(folder.size()), folder.data());
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + extent + px(8.0f)));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
    };
    // Its buttons, side by side under it.
    const auto buttons = [&](const char* first, PropertiesRequest::Kind first_kind, std::string_view path) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(20.0f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(2.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(6.0f), 0.0f));
        if (first != nullptr) {
            if (ImGui::Button(first)) {
                ask(first_kind, path);
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Show in Resources")) {
            ask(PropertiesRequest::Kind::ShowFile, path);
        }
        ImGui::PopStyleVar();
        ImGui::Dummy(ImVec2(0.0f, px(6.0f)));
    };

    const flecs::world& ecs = engine.world.ecs();
    const auto* placed = entity.try_get<world::PrefabInstance>();
    if (placed != nullptr && !sources_.prefab.empty() && begin_category("Prefab")) {
        source(sources_.prefab.view());
        const auto* prefab = engine.assets.get<asset::PrefabAsset>(placed->prefab);
        if (prefab == nullptr) {
            const PropertyRow row = begin_property_row("Parts", 0, nullptr, ThemeColor::Text, label_fraction_);
            value_label(row, "Not loaded: cook the project", ThemeColor::TextSecondary);
            end_property_row(row);
        } else {
            const std::span<const asset::PrefabVariant> variants = prefab->variants.get(prefab);
            const std::span<const asset::PrefabPart> parts = prefab->parts.get(prefab);
            // The variant it asks for: one of the prefab's, or none (all of it).
            char current[48] = "None";
            for (const asset::PrefabVariant& variant : variants) {
                if (variant.name_hash == placed->variant) {
                    const std::string_view name = prefab->name(prefab, variant);
                    static_cast<void>(
                        std::snprintf(current, sizeof(current), "%.*s", static_cast<int>(name.size()), name.data()));
                }
            }
            const PropertyRow variant_row =
                begin_property_row("Variant", 0, nullptr, ThemeColor::Text, label_fraction_);
            uint32_t chosen = placed->variant;
            ImGui::BeginDisabled(variants.empty());
            if (begin_combo("##variant", variants.empty() ? "None (it has no variants)" : current,
                            property_value_width(variant_row))) {
                if (ImGui::Selectable("None", placed->variant == 0)) {
                    chosen = 0;
                }
                for (const asset::PrefabVariant& variant : variants) {
                    const std::string_view name = prefab->name(prefab, variant);
                    char label[48];
                    static_cast<void>(
                        std::snprintf(label, sizeof(label), "%.*s", static_cast<int>(name.size()), name.data()));
                    if (ImGui::Selectable(label, variant.name_hash == placed->variant)) {
                        chosen = variant.name_hash;
                    }
                }
                end_combo();
            }
            ImGui::EndDisabled();
            if (property_label_hovered(variant_row)) {
                tooltip("The same prefab with some of its parts left out. Variants are made in the prefab editor.");
            }
            end_property_row(variant_row);
            if (chosen != placed->variant) {
                world::PrefabInstance changed = *placed;
                changed.variant = chosen;
                commands.seal();
                static_cast<void>(commands.set_component(entity, ecs.component<world::PrefabInstance>().id(),
                                                         std::as_bytes(std::span(&changed, 1))));
                commands.seal();
            }
            // Its parts: how many, and each by name with what it is, greyed where this placement's variant leaves
            // it out.
            const uint32_t left_out = prefab->variant_bit(prefab, placed->variant);
            uint32_t there = 0;
            for (const asset::PrefabPart& part : parts) {
                there += (part.absent_in & left_out) == 0 ? 1U : 0U;
            }
            char count[64];
            if (there == parts.size()) {
                static_cast<void>(std::snprintf(count, sizeof(count), "%zu", parts.size()));
            } else {
                static_cast<void>(std::snprintf(count, sizeof(count), "%u of %zu", there, parts.size()));
            }
            const PropertyRow parts_row =
                begin_property_row("Parts", 0, &parts_open_, ThemeColor::Text, label_fraction_);
            value_label(parts_row, count, ThemeColor::Text);
            end_property_row(parts_row);
            constexpr size_t kMostListed = 48;
            for (size_t p = 0; parts_open_ && p < std::min(parts.size(), kMostListed); ++p) {
                const asset::PrefabPart& part = parts[p];
                const std::string_view name = prefab->name(prefab, part);
                char label[96];
                static_cast<void>(
                    std::snprintf(label, sizeof(label), "%.*s", static_cast<int>(name.size()), name.data()));
                const bool has_mesh = part.mesh != asset::PrefabPart::kNone;
                const bool has_effect = part.effect != asset::PrefabPart::kNone;
                const bool gone = (part.absent_in & left_out) != 0;
                static constexpr const char* kKinds[8] = {
                    "Nothing", "Model",        "Effect",        "Model, effect",
                    "Light",   "Model, light", "Effect, light", "Model, effect, light"};
                const char* what = kKinds[(has_mesh ? 1U : 0U) | (has_effect ? 2U : 0U) |
                                          (part.light != asset::PrefabPart::kNone ? 4U : 0U)];
                char text[64];
                static_cast<void>(std::snprintf(text, sizeof(text), "%s%s", what, gone ? " (left out)" : ""));
                ImGui::PushID(static_cast<int>(p));
                const PropertyRow row = begin_property_row(
                    label, 1, nullptr, gone ? ThemeColor::TextDisabled : ThemeColor::Text, label_fraction_);
                value_label(row, text, gone ? ThemeColor::TextDisabled : ThemeColor::TextSecondary);
                end_property_row(row);
                ImGui::PopID();
            }
            if (parts_open_ && parts.size() > kMostListed) {
                char more[48];
                static_cast<void>(std::snprintf(more, sizeof(more), "and %zu more", parts.size() - kMostListed));
                const PropertyRow row = begin_property_row("", 1, nullptr, ThemeColor::Text, label_fraction_);
                value_label(row, more, ThemeColor::TextSecondary);
                end_property_row(row);
            }
        }
        buttons("Edit Prefab", PropertiesRequest::Kind::EditPrefab, sources_.prefab.view());
    }
    if (entity.has<world::MeshInstance>() && !sources_.mesh.empty() && begin_category("Model")) {
        source(sources_.mesh.view());
        buttons(nullptr, PropertiesRequest::Kind::None, sources_.mesh.view());
    }
    if (entity.has<world::EffectInstance>() && !sources_.effect.empty() && begin_category("Effect")) {
        source(sources_.effect.view());
        buttons("View", PropertiesRequest::Kind::ViewEffect, sources_.effect.view());
    }
}

bool PropertiesPanel::draw_file(const app::Engine& engine, const Selection& selection) {
    const std::string_view path = selection.file.view();
    const size_t slash = path.rfind('/');
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::string_view kind = file_kind(name);
    ImGui::SetCursorPos(ImVec2(px(10.0f), ImGui::GetCursorStartPos().y + px(6.0f)));
    if (fonts().semibold != nullptr) {
        ImGui::PushFont(fonts().semibold, 0.0f);
    }
    ImGui::TextColored(color(ThemeColor::TextEmphasis), "%.*s", static_cast<int>(name.size()), name.data());
    if (fonts().semibold != nullptr) {
        ImGui::PopFont();
    }
    ImGui::SetCursorPosX(px(10.0f));
    ImGui::TextColored(color(ThemeColor::TextSecondary), "%.*s", static_cast<int>(kind.size()), kind.data());
    ImGui::Dummy(ImVec2(0.0f, px(2.0f)));

    bool open = false;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    const auto text_row = [&](const char* label, const char* text, ThemeColor ink) {
        const PropertyRow row = begin_property_row(label, 0, nullptr, ThemeColor::Text, label_fraction_);
        const float start = ImGui::GetCursorScreenPos().x;
        value_label(row, text, ink);
        if (ImGui::IsMouseHoveringRect(row.min, ImVec2(row.min.x + row.width, row.min.y + row.height)) &&
            ImGui::CalcTextSize(text).x > row.value_end - start) {
            tooltip("%s", text);
        }
        end_property_row(row);
    };
    if (begin_category("File")) {
        text_row("Path", selection.file.c_str(), ThemeColor::Text);
        char size[32] = "?";
        if (const auto bytes = engine.vfs.file_size(selection.file.c_str()); bytes) {
            static_cast<void>(std::snprintf(size, sizeof(size), "%.1f KB", static_cast<double>(*bytes) / 1024.0));
        }
        text_row("Size", size, ThemeColor::Text);
    }
    asset::Guid guid;
    asset::AssetId id;
    const bool has_meta = source_asset_id(engine.vfs, path, guid, id);
    if (begin_category("Asset")) {
        if (has_meta) {
            text_row("Asset ID", asset::format_asset_id(id).c_str(), ThemeColor::Text);
        } else {
            text_row("Asset ID", "No meta yet: the cooker makes one", ThemeColor::TextSecondary);
        }
        if (has_meta && (kind == "World" || kind == "Effect")) {
            // A world opens in place of the one shown; an effect in the effect viewer.
            const PropertyRow row =
                begin_property_row(kind == "World" ? "World" : "Effect", 0, nullptr, ThemeColor::Text, label_fraction_);
            open = ImGui::Button(kind == "World" ? "Open" : "View",
                                 ImVec2(std::min(px(96.0f), property_value_width(row)), 0.0f));
            end_property_row(row);
        }
    }
    ImGui::PopStyleVar();
    return open;
}

void PropertiesPanel::draw_terrain(const app::Engine& engine) {
    const auto* terrain = engine.assets.get<asset::TerrainAsset>(engine.terrain);
    if (terrain == nullptr) {
        empty_message("This world has no terrain.");
        return;
    }
    ImGui::SetCursorPos(ImVec2(px(10.0f), ImGui::GetCursorStartPos().y + px(6.0f)));
    if (fonts().semibold != nullptr) {
        ImGui::PushFont(fonts().semibold, 0.0f);
    }
    ImGui::TextColored(color(ThemeColor::TextEmphasis), "Terrain");
    if (fonts().semibold != nullptr) {
        ImGui::PopFont();
    }
    ImGui::SetCursorPosX(px(10.0f));
    ImGui::TextColored(color(ThemeColor::TextSecondary), "The world's ground: it stays where it is");
    ImGui::Dummy(ImVec2(0.0f, px(2.0f)));
    // How high it reaches: its patches' bounds.
    float lowest = 0.0f;
    float highest = 0.0f;
    bool any = false;
    for (const asset::TerrainPatch& patch : terrain->patches.get(terrain)) {
        lowest = any ? std::min(lowest, patch.min_m) : patch.min_m;
        highest = any ? std::max(highest, patch.max_m) : patch.max_m;
        any = true;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    const auto text_row = [&](const char* label, const char* text) {
        const PropertyRow row = begin_property_row(label, 0, nullptr, ThemeColor::Text, label_fraction_);
        value_label(row, text, ThemeColor::Text);
        end_property_row(row);
    };
    char text[64];
    if (begin_category("Terrain")) {
        text_row("Asset ID", asset::format_asset_id(engine.assets.asset(engine.terrain).id).c_str());
        static_cast<void>(std::snprintf(text, sizeof(text), "%.2f x %.2f km",
                                        static_cast<double>(terrain->width_m) / 1000.0,
                                        static_cast<double>(terrain->depth_m) / 1000.0));
        text_row("Size", text);
        static_cast<void>(
            std::snprintf(text, sizeof(text), "%.1f, %.1f m", terrain->origin_m[0], terrain->origin_m[1]));
        text_row("Origin (X, Z)", text);
        if (any) {
            static_cast<void>(std::snprintf(text, sizeof(text), "%.1f to %.1f m", static_cast<double>(lowest),
                                            static_cast<double>(highest)));
            text_row("Height bounds", text);
        }
        static_cast<void>(std::snprintf(text, sizeof(text), "%u", terrain->levels.count));
        text_row("Levels", text);
    }
    ImGui::PopStyleVar();
}

bool PropertiesPanel::draw(const app::Engine& engine, Selection& selection, CommandStack& commands) {
    DE_PROFILE_ZONE("PropertiesPanel::draw");
    bool open = false;
    if (ImGui::Begin(kWindow)) {
        switch (selection.kind) {
        case Selection::Kind::Entity:
            draw_entity(engine, selection, commands);
            break;
        case Selection::Kind::Terrain:
            draw_terrain(engine);
            break;
        case Selection::Kind::File:
            open = draw_file(engine, selection);
            break;
        case Selection::Kind::None:
            empty_message("Select an entity or a resource to see its properties.");
            break;
        }
    }
    ImGui::End();
    return open;
}

} // namespace de::editor
