#pragma once

#include "core/math.h"
#include "world/transform.h"

#include <flecs.h>
#include <imgui.h>

#include <cstdint>
#include <span>

namespace de::editor {

class CommandStack;

// What a drag in the Playground does to the selection.
enum class Tool : uint8_t { Select, Move, Rotate, Scale };
// Which axes the handles follow: the world's, or the entity's own.
enum class Space : uint8_t { World, Local };
// Where the handles sit, and what turning and scaling go about: the entity's own origin (its pivot), or the centre of
// its geometry's bounds (with what's attached to it).
enum class HandlesAt : uint8_t { Pivot, Center };

// Snapping: moves to a grid, turns in steps of so many degrees, scales in steps.
struct Snap {
    bool move = false;
    float move_m = 0.5f;
    bool rotate = false;
    float rotate_deg = 15.0f;
    bool scale = false;
    float scale_step = 0.1f;
};

// How the Playground sees the world: the camera, and the rectangle on screen the scene fills.
struct View {
    DVec3 position{0.0};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float vertical_fov_rad = 1.0f;
    ImVec2 min{0.0f, 0.0f};
    ImVec2 size{1.0f, 1.0f};

    // Where a point is on screen; false if it's behind the camera.
    bool project(const DVec3& point, ImVec2& out) const;
    // The direction from the camera through a point on screen, in the world (unit length).
    Vec3 ray(ImVec2 screen) const;
    // How many meters a pixel covers at a point's distance from the camera.
    float meters_per_pixel(const DVec3& point) const;
};

// Where a ray (from origin, along direction, both relative to a line's point) comes closest to the line through that
// point along axis (unit length): how far along the axis. False when the ray runs along the axis.
bool closest_on_axis(const Vec3& origin, const Vec3& direction, const Vec3& axis, float& along);

// A placement turned by turn about a point: its rotation, and its position around the point.
world::WorldTransform turned_about(const world::WorldTransform& start, const DVec3& point, const Quat& turn);
// The angle to turn a placement by about an axis (unit length) so that, snapped to step, its own turn about that axis
// (its twist) lands on a whole step: a quarter turn of snapping at 15 degrees takes 40 degrees to 60, not 55.
float snapped_turn(const Quat& start, const Vec3& axis, float angle_rad, float step_rad);
// A placement scaled by factors along its own axes (axes: those axes in the world, unit length), about a point: its
// scale, and its position's offset from the point along each axis.
world::WorldTransform scaled_about(const world::WorldTransform& start, const DVec3& point,
                                   std::span<const Vec3, 3> axes, const Vec3& factors);

// The handles on the selected entity, drawn over the Playground, as Unreal Engine 4's: in the axis colours, yellow
// under the mouse, the same size on screen at any distance. Moving: an arrow along each axis (a shaded cone at its
// tip), a corner between each two axes to move in their plane, a sphere in the middle to move in the screen's plane;
// a move under way shows only what it goes along, where it began, and how far in centimetres. Turning: a ring round
// each axis about a sphere, the half facing the camera a band (tapering where it turns away), the far half a thin
// line; a drag along a band turns about its axis, and shows on that ring in the band's style: the wedge swept, a ruler
// round the ring from the grab, the ways to drag at the grab, and the angle in degrees. A drag inside the sphere rolls
// the selection any way, as a ball under the mouse. Scaling:
// the moving handles' layout with a cube for each cone, a triangle across each corner to scale two axes, a cube in the
// middle to scale it whole (always along its own axes). Each drag is one command; snapping rounds what it does. An
// attached entity keeps its parent: what changes is its place relative to it.
class Gizmo {
public:
    // Draws the tool's handles at origin (the entity's pivot or its geometry's centre: HandlesAt) and runs a drag.
    // mouse_free: the mouse is over the scene and nothing else has it. True while the mouse is on a handle or dragging
    // one: a click there selects nothing else.
    bool update(const View& view, ImDrawList* draw, Tool tool, Space space, const Snap& snap, flecs::entity entity,
                const DVec3& origin, bool mouse_free, CommandStack& commands);
    bool dragging() const { return handle_ >= 0; }
    // The handle under the mouse in the last update (as kPlane and kMiddle number them), or -1.
    int hovered_handle() const { return hovered_; }
    // For pictures (the editor's --drag): from the next update with an entity, a drag on that handle shows as under
    // way, gone so far (meters moving, radians turning), and stays so without changing anything.
    void show_drag(int handle, float amount) {
        preview_ = Preview::Asked;
        preview_handle_ = handle;
        preview_amount_ = amount;
    }

    // The handles: 0 to 2 an axis; 3 to 5 the plane at right angles to axis (handle - 3); 6 the middle.
    static constexpr int kPlane = 3;
    static constexpr int kMiddle = 6;

private:
    int handle_ = -1; // being dragged
    flecs::entity_t entity_ = 0;
    world::WorldTransform start_;
    DVec3 start_origin_{0.0};
    Vec3 axes_[3] = {}; // the axes at the drag's start
    float start_along_ = 0.0f;
    Vec3 start_hit_{0.0f};       // moving in a plane: where the drag began on it, from the origin
    Vec3 grab_{0.0f};            // turning: from the origin to where the ring was grabbed, unit length
    ImVec2 tangent_{1.0f, 0.0f}; // turning: the way along the ring on screen at the grab, for a positive angle
    ImVec2 start_mouse_{0.0f, 0.0f};
    float angle_rad_ = 0.0f; // turning: the angle so far, snapped
    Vec3 moved_{0.0f};       // moving: how far so far, snapped
    Vec3 scaled_{1.0f};      // scaling: by how much along each of its axes so far, snapped
    int hovered_ = -1;
    uint64_t merge_key_ = 0;
    uint32_t drags_ = 0;
    enum class Preview : uint8_t { None, Asked, Showing };
    Preview preview_ = Preview::None;
    int preview_handle_ = 0;
    float preview_amount_ = 0.0f;
};

} // namespace de::editor
