#include "editor/cell_map_panel.h"

#include "app/framework.h"
#include "asset/terrain_format.h"
#include "core/profile.h"
#include "editor/theme.h"
#include "editor/widgets.h"
#include "world/cell.h"
#include "world/render_extract.h"
#include "world/transform.h"
#include "world/world.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace de::editor {

namespace {

constexpr float kMinPixelsPerM = 0.002f; // about 30 km across a panel
constexpr float kMaxPixelsPerM = 8.0f;

// The cell a world position is in.
world::CellCoord cell_of(double x_m, double z_m) {
    return {.x = static_cast<int32_t>(std::floor(x_m / world::kCellSize)),
            .z = static_cast<int32_t>(std::floor(z_m / world::kCellSize))};
}

} // namespace

bool CellMapPanel::draw(const app::Engine& engine, Selection& selection, DVec3& to) {
    DE_PROFILE_ZONE("CellMapPanel::draw");
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f)); // the map edge to edge
    const bool shown = ImGui::Begin(kWindow);
    ImGui::PopStyleVar();
    if (!shown) {
        focused_ = false;
        ImGui::End();
        return false;
    }
    const flecs::world& ecs = engine.world.ecs();
    // The camera: where it is and where it looks.
    bool have_camera = false;
    DVec3 camera(0.0);
    Vec3 forward(0.0f, 0.0f, -1.0f);
    if (const auto* active = ecs.try_get<world::ActiveCamera>(); active != nullptr && ecs.is_alive(active->entity)) {
        if (const auto* place = ecs.entity(active->entity).try_get<world::WorldTransform>()) {
            have_camera = true;
            camera = place->position;
            forward = place->rotation * Vec3(0.0f, 0.0f, -1.0f);
        }
    }
    if (follow_ && have_camera) {
        center_x_m_ = camera.x;
        center_z_m_ = camera.z;
    }

    // The canvas: the whole panel. X goes right, -Z up: north is where the camera looks at first.
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(1.0f, ImGui::GetContentRegionAvail().x),
                      std::max(1.0f, ImGui::GetContentRegionAvail().y));
    const ImVec2 max(min.x + size.x, min.y + size.y);
    const ImVec2 middle(min.x + (size.x * 0.5f), min.y + (size.y * 0.5f));
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##map", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiIO& io = ImGui::GetIO();
    const auto to_screen = [&](double x_m, double z_m) {
        return ImVec2(middle.x + static_cast<float>((x_m - center_x_m_) * pixels_per_m_),
                      middle.y + static_cast<float>((z_m - center_z_m_) * pixels_per_m_));
    };
    const auto to_world_x = [&](float x) { return center_x_m_ + static_cast<double>((x - middle.x) / pixels_per_m_); };
    const auto to_world_z = [&](float y) { return center_z_m_ + static_cast<double>((y - middle.y) / pixels_per_m_); };

    // Zoom about the mouse; drag with the right or middle button.
    if (hovered && io.MouseWheel != 0.0f) {
        const double under_x = to_world_x(io.MousePos.x);
        const double under_z = to_world_z(io.MousePos.y);
        pixels_per_m_ = std::clamp(pixels_per_m_ * std::pow(1.2f, io.MouseWheel), kMinPixelsPerM, kMaxPixelsPerM);
        center_x_m_ = under_x - static_cast<double>((io.MousePos.x - middle.x) / pixels_per_m_);
        center_z_m_ = under_z - static_cast<double>((io.MousePos.y - middle.y) / pixels_per_m_);
        follow_ = false;
    }
    if (ImGui::IsItemActive() &&
        (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
        center_x_m_ -= static_cast<double>(io.MouseDelta.x / pixels_per_m_);
        center_z_m_ -= static_cast<double>(io.MouseDelta.y / pixels_per_m_);
        follow_ = false;
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(min, max, true);
    draw->AddRectFilled(min, max, color_u32(ThemeColor::Surface2));

    // The terrain's extent: the land.
    if (const auto* terrain = engine.assets.get<asset::TerrainAsset>(engine.terrain)) {
        const ImVec2 land_min = to_screen(terrain->origin_m[0], terrain->origin_m[1]);
        const ImVec2 land_max = to_screen(terrain->origin_m[0] + static_cast<double>(terrain->width_m),
                                          terrain->origin_m[1] + static_cast<double>(terrain->depth_m));
        draw->AddRectFilled(land_min, land_max, color_u32(ThemeColor::Surface));
        draw->AddRect(land_min, land_max, color_u32(ThemeColor::Control));
    }

    // The grid, where its lines are far enough apart to read.
    const auto cell_px = static_cast<float>(world::kCellSize * static_cast<double>(pixels_per_m_));
    if (cell_px >= 6.0f) {
        const world::CellCoord first = cell_of(to_world_x(min.x), to_world_z(min.y));
        const world::CellCoord last = cell_of(to_world_x(max.x), to_world_z(max.y));
        const ImU32 line = color_u32(ThemeColor::Header);
        for (int32_t x = first.x; x <= last.x + 1; ++x) {
            const float sx = to_screen(static_cast<double>(x) * world::kCellSize, 0.0).x;
            draw->AddLine(ImVec2(sx, min.y), ImVec2(sx, max.y), line);
        }
        for (int32_t z = first.z; z <= last.z + 1; ++z) {
            const float sy = to_screen(0.0, static_cast<double>(z) * world::kCellSize).y;
            draw->AddLine(ImVec2(min.x, sy), ImVec2(max.x, sy), line);
        }
    }

    // The cells that are loaded; the one under the mouse, and the one selected.
    const world::CellCoord mouse_cell = cell_of(to_world_x(io.MousePos.x), to_world_z(io.MousePos.y));
    flecs::entity hovered_cell;
    uint32_t loaded = 0;
    ecs.each([&](flecs::entity cell, const world::Cell& data) {
        ++loaded;
        const ImVec2 a = to_screen(static_cast<double>(data.coord.x) * world::kCellSize,
                                   static_cast<double>(data.coord.z) * world::kCellSize);
        const ImVec2 b = to_screen(static_cast<double>(data.coord.x + 1) * world::kCellSize,
                                   static_cast<double>(data.coord.z + 1) * world::kCellSize);
        const bool selected = selection.kind == Selection::Kind::Entity && selection.entity == cell.id();
        const bool under = hovered && data.coord.x == mouse_cell.x && data.coord.z == mouse_cell.z;
        if (under) {
            hovered_cell = cell;
        }
        draw->AddRectFilled(a, b, color_u32(ThemeColor::Selection, under ? 0.6f : 0.4f));
        draw->AddRect(a, b, color_u32(selected ? ThemeColor::Accent : ThemeColor::Selection), 0.0f, 0,
                      selected ? 2.0f : 1.0f);
        if (b.x - a.x >= ImGui::CalcTextSize("-00, -00").x + px(8.0f)) {
            char label[32];
            static_cast<void>(std::snprintf(label, sizeof(label), "%d, %d", data.coord.x, data.coord.z));
            draw->AddText(ImVec2(a.x + px(4.0f), a.y + px(2.0f)), color_u32(ThemeColor::TextSecondary), label);
        }
    });

    // The selection: a small diamond where it is.
    if (selection.kind == Selection::Kind::Entity && ecs.is_alive(selection.entity)) {
        if (const auto* place = ecs.entity(selection.entity).try_get<world::WorldTransform>()) {
            const ImVec2 at = to_screen(place->position.x, place->position.z);
            const float r = px(4.0f);
            draw->AddQuadFilled(ImVec2(at.x, at.y - r), ImVec2(at.x + r, at.y), ImVec2(at.x, at.y + r),
                                ImVec2(at.x - r, at.y), color_u32(ThemeColor::TextEmphasis));
        }
    }
    // The camera: a dot, and a line where it looks.
    if (have_camera) {
        const ImVec2 at = to_screen(camera.x, camera.z);
        const Vec2 look = glm::length(Vec2(forward.x, forward.z)) > 1e-4f ? glm::normalize(Vec2(forward.x, forward.z))
                                                                          : Vec2(0.0f, -1.0f);
        draw->AddLine(at, ImVec2(at.x + (look.x * px(18.0f)), at.y + (look.y * px(18.0f))),
                      color_u32(ThemeColor::Accent), std::max(1.0f, px(1.5f)));
        draw->AddCircleFilled(at, px(4.0f), color_u32(ThemeColor::Accent));
    }

    // Center: back on the camera, when the view has left it.
    bool recenter = false;
    if (!follow_) {
        const char* text = "Center";
        const ImVec2 text_size = ImGui::CalcTextSize(text);
        const ImVec2 button_min(max.x - text_size.x - px(20.0f), min.y + px(6.0f));
        ImGui::SetCursorScreenPos(button_min);
        recenter = ImGui::Button(text);
        if (ImGui::IsItemHovered()) {
            tooltip("Follow the camera again");
        }
    }
    draw->PopClipRect();

    // A click selects a cell; a double-click sends the camera there.
    bool go = false;
    if (hovered && !recenter && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hovered_cell.is_valid()) {
            selection.select_entity(hovered_cell.id());
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            to = DVec3(to_world_x(io.MousePos.x), camera.y, to_world_z(io.MousePos.y));
            go = true;
            follow_ = true;
        }
    }
    if (hovered && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        if (hovered_cell.is_valid()) {
            uint32_t members = 0;
            ecs.each(ecs.pair<world::InCell>(hovered_cell), [&members](flecs::entity) { ++members; });
            tooltip("Cell [%d, %d]: %u object%s, loaded", mouse_cell.x, mouse_cell.z, members, members == 1 ? "" : "s");
        } else {
            tooltip("Cell [%d, %d]: empty", mouse_cell.x, mouse_cell.z);
        }
    }
    if (recenter) {
        follow_ = true;
    }

    const world::CellCoord camera_cell = cell_of(camera.x, camera.z);
    static_cast<void>(std::snprintf(status_, sizeof(status_), "%u cell%s loaded  camera in cell [%d, %d]", loaded,
                                    loaded == 1 ? "" : "s", camera_cell.x, camera_cell.z));
    focused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();
    return go;
}

} // namespace de::editor
