#include "editor/gizmo.h"

#include "core/hash.h"
#include "editor/commands.h"
#include "editor/theme.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <span>

namespace de::editor {

namespace {

// Sizes on screen at 100%, in pixels.
constexpr float kLengthPx = 88.0f;         // an axis's shaft: to its cone's base, or its cube's centre
constexpr float kShaftPx = 4.0f;           // the lines' width
constexpr float kConeLengthPx = 27.0f;     // the arrowheads
constexpr float kConeRadiusPx = 8.5f;      //
constexpr float kCubePx = 7.5f;            // half a scaling cube's side
constexpr float kMiddlePx = 7.0f;          // the middle sphere's radius, the middle cube's half side
constexpr float kCornerShare = 0.3f;       // moving: a plane's corner, as a share of the axis
constexpr float kScaleCornerShare = 0.55f; // scaling: the triangle across a corner, as a share of the axis
constexpr float kRingPx = 108.0f;          // the rings' radius: the sphere they lie on
constexpr float kBandPx = 8.0f;            // a ring's band where it faces the camera, as wide however it's turned
constexpr float kBackPx = 1.5f;            // a ring's far half: a line
constexpr float kGrabPx = 9.0f;            // how near the mouse must come
constexpr int kRingSteps = 128;
constexpr int kConeSides = 24;

// Unreal's colours: saturated axes, lighter rings, yellow under the mouse or dragged, a white middle.
constexpr ImU32 kAxisInk[] = {IM_COL32(228, 48, 30, 255), IM_COL32(108, 196, 36, 255), IM_COL32(38, 116, 250, 255)};
constexpr ImU32 kRingInk[] = {IM_COL32(240, 72, 52, 255), IM_COL32(124, 212, 54, 255), IM_COL32(62, 138, 255, 255)};
constexpr ImU32 kHot = IM_COL32(255, 228, 32, 255);
constexpr ImU32 kSweepInk = IM_COL32(255, 255, 255, 255); // a turn's progress over its yellow band
// Every handle's dark edge: they read on bright sand and sky as on dark ground.
constexpr ImU32 kOutline = IM_COL32(10, 10, 12, 135);
constexpr ImU32 kMiddleInk = IM_COL32(246, 246, 246, 255);

// The point on a segment nearest p: how far it is, and where along (0 to 1).
float distance_to_segment(ImVec2 p, ImVec2 a, ImVec2 b, float* along = nullptr) {
    const ImVec2 ab(b.x - a.x, b.y - a.y);
    const float length2 = (ab.x * ab.x) + (ab.y * ab.y);
    const float t =
        length2 > 0.0f ? std::clamp((((p.x - a.x) * ab.x) + ((p.y - a.y) * ab.y)) / length2, 0.0f, 1.0f) : 0.0f;
    if (along != nullptr) {
        *along = t;
    }
    const float dx = p.x - (a.x + (ab.x * t));
    const float dy = p.y - (a.y + (ab.y * t));
    return std::sqrt((dx * dx) + (dy * dy));
}

// The unit normal on screen of the way from one point to another (left of it, with y down); none if they're one point.
ImVec2 normal_of(ImVec2 from, ImVec2 to) {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float length = std::sqrt((dx * dx) + (dy * dy));
    return length > 1e-4f ? ImVec2(-dy / length, dx / length) : ImVec2(0.0f, 0.0f);
}

// 0 below low, 1 above high, smooth between.
float smooth_step(float low, float high, float x) {
    const float t = std::clamp((x - low) / (high - low), 0.0f, 1.0f);
    return t * t * (3.0f - (2.0f * t));
}

float signed_area(std::span<const ImVec2> points) {
    float area = 0.0f;
    for (size_t i = 0; i < points.size(); ++i) {
        const ImVec2& a = points[i];
        const ImVec2& b = points[(i + 1) % points.size()];
        area += (a.x * b.y) - (b.x * a.y);
    }
    return area * 0.5f;
}

bool inside_convex(ImVec2 p, std::span<const ImVec2> points) {
    float sign = 0.0f;
    for (size_t i = 0; i < points.size(); ++i) {
        const ImVec2& a = points[i];
        const ImVec2& b = points[(i + 1) % points.size()];
        const float side = ((b.x - a.x) * (p.y - a.y)) - ((b.y - a.y) * (p.x - a.x));
        if (side != 0.0f) {
            if (sign != 0.0f && (side > 0.0f) != (sign > 0.0f)) {
                return false;
            }
            sign = side;
        }
    }
    return true;
}

// A convex polygon, filled: ImGui's anti-aliased edge wants its points clockwise on screen.
void fill_convex(ImDrawList* draw, std::span<ImVec2> points, ImU32 color) {
    if (signed_area(points) < 0.0f) {
        std::ranges::reverse(points);
    }
    draw->AddConvexPolyFilled(points.data(), static_cast<int>(points.size()), color);
}

// The convex hull of a few points on screen (Andrew's monotone chain), into hull; how many points it has.
size_t convex_hull(std::span<ImVec2> points, std::span<ImVec2> hull) {
    if (points.size() < 3 || hull.size() < 2 * points.size()) {
        return 0;
    }
    std::ranges::sort(points, [](ImVec2 a, ImVec2 b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
    const auto turn = [](ImVec2 o, ImVec2 a, ImVec2 b) {
        return ((a.x - o.x) * (b.y - o.y)) - ((a.y - o.y) * (b.x - o.x));
    };
    size_t count = 0;
    for (const ImVec2 p : points) {
        while (count >= 2 && turn(hull[count - 2], hull[count - 1], p) <= 0.0f) {
            --count;
        }
        hull[count++] = p;
    }
    const size_t lower = count + 1;
    for (size_t i = points.size() - 1; i-- > 0;) {
        while (count >= lower && turn(hull[count - 2], hull[count - 1], points[i]) <= 0.0f) {
            --count;
        }
        hull[count++] = points[i];
    }
    return count > 1 ? count - 1 : count;
}

ImU32 shaded(ImU32 color, float light) {
    const auto channel = [&](unsigned shift) {
        const float value = static_cast<float>((color >> shift) & 0xFFU) * light;
        return static_cast<ImU32>(std::clamp(value, 0.0f, 255.0f)) << shift;
    };
    return channel(IM_COL32_R_SHIFT) | channel(IM_COL32_G_SHIFT) | channel(IM_COL32_B_SHIFT) |
           (color & IM_COL32_A_MASK);
}

ImU32 faded(ImU32 color, float alpha) {
    const auto a = static_cast<ImU32>(std::clamp(alpha, 0.0f, 1.0f) * static_cast<float>(color >> IM_COL32_A_SHIFT));
    return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

// How much wider a line's dark edge is than the line.
float outline_extra() {
    return std::max(1.5f, px(2.0f));
}

// A line with a dark edge either side.
void outlined_line(ImDrawList* draw, ImVec2 a, ImVec2 b, ImU32 color, float width) {
    draw->AddLine(a, b, kOutline, width + outline_extra());
    draw->AddLine(a, b, color, width);
}

// A dashed line, each dash edged dark.
void dashed_line(ImDrawList* draw, ImVec2 a, ImVec2 b, ImU32 color, float width) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float length = std::sqrt((dx * dx) + (dy * dy));
    const float dash = px(7.0f);
    const float gap = px(5.0f);
    const auto dashes = static_cast<int>(std::ceil(length / (dash + gap)));
    for (int k = 0; k < dashes; ++k) {
        const float along = static_cast<float>(k) * (dash + gap);
        const float from = along / length;
        const float to = std::min(along + dash, length) / length;
        outlined_line(draw, ImVec2(a.x + (dx * from), a.y + (dy * from)), ImVec2(a.x + (dx * to), a.y + (dy * to)),
                      color, width);
    }
}

// Where a drag began: a small yellow ring, edged dark.
void start_marker(ImDrawList* draw, ImVec2 at) {
    draw->AddCircleFilled(at, px(5.5f), kOutline, 24);
    draw->AddCircle(at, px(4.0f), kHot, 24, std::max(1.0f, px(2.0f)));
}

// A value beside the handles (how far a drag has gone), on a dark box centred on a point.
void label(ImDrawList* draw, ImVec2 middle, const char* text) {
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 corner(std::floor(middle.x - (size.x * 0.5f)), std::floor(middle.y - (size.y * 0.5f)));
    const ImVec2 pad(px(6.0f), px(3.0f));
    draw->AddRectFilled(ImVec2(corner.x - pad.x, corner.y - pad.y),
                        ImVec2(corner.x + size.x + pad.x, corner.y + size.y + pad.y), IM_COL32(18, 18, 20, 225),
                        px(3.0f));
    draw->AddRect(ImVec2(corner.x - pad.x, corner.y - pad.y),
                  ImVec2(corner.x + size.x + pad.x, corner.y + size.y + pad.y), IM_COL32(255, 255, 255, 40), px(3.0f));
    draw->AddText(corner, color_u32(ThemeColor::TextEmphasis), text);
}

// Where a ray (relative to a plane's point) meets the plane through that point; false if it runs along it or away.
bool hit_plane(const Vec3& origin, const Vec3& direction, const Vec3& normal, Vec3& hit) {
    const float facing = glm::dot(direction, normal);
    if (std::abs(facing) < 1e-4f) {
        return false;
    }
    const float t = -glm::dot(origin, normal) / facing;
    if (t <= 0.0f) {
        return false;
    }
    hit = origin + (direction * t);
    return true;
}

float snapped(float value, bool snap, float step) {
    return snap && step > 0.0f ? std::round(value / step) * step : value;
}

// The entity's parent's world transform, or null for a root.
const world::WorldTransform* parent_world(flecs::entity entity) {
    const flecs::world ecs = entity.world();
    flecs::entity parent;
    if (const auto* attached = entity.try_get<flecs::Parent>(); attached != nullptr && ecs.is_alive(attached->value)) {
        parent = ecs.entity(attached->value);
    } else {
        parent = entity.parent();
    }
    return parent.is_valid() ? parent.try_get<world::WorldTransform>() : nullptr;
}

// Draws shaded solids over the Playground: faces lit from over the camera's shoulder, the silhouette anti-aliased
// (faces inside it without, so no seams show between them).
struct Painter {
    const View& view;
    ImDrawList* draw;
    Vec3 light;

    float lit(const Vec3& normal) const { return 0.55f + (0.5f * std::max(0.0f, glm::dot(normal, light))); }
    bool faces_camera(const DVec3& point, const Vec3& normal) const {
        return glm::dot(normal, Vec3(view.position - point)) > 0.0f;
    }

    // The outline of some points on screen.
    struct Hull {
        ImVec2 points[2 * (kConeSides + 2)];
        size_t count = 0;
    };

    // The outline of these points, filled anti-aliased in color under the faces.
    Hull silhouette(std::span<ImVec2> points, ImU32 color) const {
        Hull hull;
        hull.count = convex_hull(points, std::span(hull.points, std::min(std::size(hull.points), 2 * points.size())));
        if (hull.count >= 3) {
            fill_convex(draw, std::span(hull.points, hull.count), color);
        }
        return hull;
    }

    // The outline edged dark, over the faces.
    void edge(const Hull& hull) const {
        if (hull.count >= 3) {
            draw->AddPolyline(hull.points, static_cast<int>(hull.count), kOutline, ImDrawFlags_Closed,
                              std::max(1.0f, px(1.25f)));
        }
    }

    void faces_begin() const { draw->Flags &= ~ImDrawListFlags_AntiAliasedFill; }
    void faces_end() const { draw->Flags |= ImDrawListFlags_AntiAliasedFill; }

    void cone(const DVec3& base, const Vec3& axis, float length_m, float radius_m, ImU32 color) const {
        const Vec3 u = std::abs(axis.y) < 0.9f ? glm::normalize(glm::cross(axis, Vec3(0.0f, 1.0f, 0.0f)))
                                               : glm::normalize(glm::cross(axis, Vec3(1.0f, 0.0f, 0.0f)));
        const Vec3 w = glm::cross(axis, u);
        const DVec3 apex = base + DVec3(axis * length_m);
        ImVec2 points[kConeSides + 1];
        Vec3 sides[kConeSides];
        if (!view.project(apex, points[kConeSides])) {
            return;
        }
        for (int s = 0; s < kConeSides; ++s) {
            const float t = 2.0f * std::numbers::pi_v<float> * static_cast<float>(s) / kConeSides;
            sides[s] = (u * std::cos(t)) + (w * std::sin(t));
            if (!view.project(base + DVec3(sides[s] * radius_m), points[s])) {
                return;
            }
        }
        ImVec2 outline[kConeSides + 1];
        std::ranges::copy(points, outline);
        const Hull hull = silhouette(outline, shaded(color, 0.8f));
        faces_begin();
        if (faces_camera(base, -axis)) {
            ImVec2 rim[kConeSides];
            std::ranges::copy(std::span(points, kConeSides), rim);
            fill_convex(draw, rim, shaded(color, lit(-axis)));
        }
        for (int s = 0; s < kConeSides; ++s) {
            const int next = (s + 1) % kConeSides;
            const Vec3 middle = glm::normalize(sides[s] + sides[next]);
            const Vec3 normal = glm::normalize((middle * length_m) + (axis * radius_m));
            if (faces_camera(base + DVec3(middle * radius_m), normal)) {
                ImVec2 face[3] = {points[kConeSides], points[s], points[next]};
                fill_convex(draw, face, shaded(color, lit(normal)));
            }
        }
        faces_end();
        edge(hull);
    }

    void cube(const DVec3& center, std::span<const Vec3, 3> axes, float half_m, ImU32 color) const {
        ImVec2 corners[8];
        for (int c = 0; c < 8; ++c) {
            const Vec3 offset = (axes[0] * ((c & 1) != 0 ? half_m : -half_m)) +
                                (axes[1] * ((c & 2) != 0 ? half_m : -half_m)) +
                                (axes[2] * ((c & 4) != 0 ? half_m : -half_m));
            if (!view.project(center + DVec3(offset), corners[c])) {
                return;
            }
        }
        ImVec2 outline[8];
        std::ranges::copy(corners, outline);
        const Hull hull = silhouette(outline, shaded(color, 0.8f));
        faces_begin();
        for (int k = 0; k < 3; ++k) {
            const int i = (k + 1) % 3;
            const int j = (k + 2) % 3;
            for (const float sign : {-1.0f, 1.0f}) {
                const Vec3 normal = axes[k] * sign;
                if (!faces_camera(center + DVec3(normal * half_m), normal)) {
                    continue;
                }
                // The face's corners in order around it: bit k fixed by the sign, bits i and j round the square.
                const int fixed = sign > 0.0f ? 1 << k : 0;
                ImVec2 face[4] = {corners[fixed], corners[fixed | (1 << i)], corners[fixed | (1 << i) | (1 << j)],
                                  corners[fixed | (1 << j)]};
                fill_convex(draw, face, shaded(color, lit(normal)));
            }
        }
        faces_end();
        edge(hull);
    }

    // A sphere seen from anywhere: a disc, lighter up toward the light.
    void sphere(ImVec2 center, float radius_px, ImU32 color) const {
        draw->AddCircleFilled(center, radius_px, shaded(color, 0.72f), 32);
        const ImVec2 toward(center.x - (radius_px * 0.22f), center.y - (radius_px * 0.22f));
        draw->AddCircleFilled(toward, radius_px * 0.62f, shaded(color, 0.95f), 32);
        draw->AddCircle(center, radius_px, kOutline, 32, std::max(1.0f, px(1.25f)));
    }
};

} // namespace

bool View::project(const DVec3& point, ImVec2& out) const {
    const Vec3 view = glm::conjugate(rotation) * Vec3(point - position);
    if (view.z > -1e-3f) {
        return false;
    }
    const float tan_half = std::tan(vertical_fov_rad * 0.5f);
    const float aspect = size.x / std::max(size.y, 1.0f);
    const float x = view.x / (-view.z * tan_half * aspect);
    const float y = view.y / (-view.z * tan_half);
    out = ImVec2(min.x + (((x * 0.5f) + 0.5f) * size.x), min.y + ((0.5f - (y * 0.5f)) * size.y));
    return true;
}

Vec3 View::ray(ImVec2 screen) const {
    const float tan_half = std::tan(vertical_fov_rad * 0.5f);
    const float aspect = size.x / std::max(size.y, 1.0f);
    const float x = (((screen.x - min.x) / std::max(size.x, 1.0f)) * 2.0f) - 1.0f;
    const float y = 1.0f - (((screen.y - min.y) / std::max(size.y, 1.0f)) * 2.0f);
    return glm::normalize(rotation * Vec3(x * tan_half * aspect, y * tan_half, -1.0f));
}

float View::meters_per_pixel(const DVec3& point) const {
    const Vec3 view = glm::conjugate(rotation) * Vec3(point - position);
    const float depth = std::max(-view.z, 1e-3f);
    return 2.0f * depth * std::tan(vertical_fov_rad * 0.5f) / std::max(size.y, 1.0f);
}

bool closest_on_axis(const Vec3& origin, const Vec3& direction, const Vec3& axis, float& along) {
    // The line is the axis through the origin of these coordinates; the ray starts at origin.
    const float b = glm::dot(axis, direction);
    const float denominator = 1.0f - (b * b);
    if (denominator < 1e-4f) {
        return false;
    }
    const Vec3 w = -origin;
    along = ((b * glm::dot(direction, w)) - glm::dot(axis, w)) / denominator;
    return true;
}

world::WorldTransform turned_about(const world::WorldTransform& start, const DVec3& point, const Quat& turn) {
    world::WorldTransform turned = start;
    turned.rotation = glm::normalize(turn * start.rotation);
    turned.position = point + DVec3(turn * Vec3(start.position - point));
    return turned;
}

float snapped_turn(const Quat& start, const Vec3& axis, float angle_rad, float step_rad) {
    if (step_rad <= 0.0f) {
        return angle_rad;
    }
    // Its twist about the axis, between a half turn back and one on.
    constexpr float kPi = std::numbers::pi_v<float>;
    float twist = 2.0f * std::atan2(glm::dot(Vec3(start.x, start.y, start.z), axis), start.w);
    twist = twist > kPi ? twist - (2.0f * kPi) : (twist < -kPi ? twist + (2.0f * kPi) : twist);
    return (std::round((twist + angle_rad) / step_rad) * step_rad) - twist;
}

world::WorldTransform scaled_about(const world::WorldTransform& start, const DVec3& point,
                                   std::span<const Vec3, 3> axes, const Vec3& factors) {
    world::WorldTransform scaled = start;
    scaled.scale = start.scale * factors;
    const Vec3 offset(start.position - point);
    Vec3 moved(0.0f);
    for (int k = 0; k < 3; ++k) {
        moved += axes[k] * (glm::dot(offset, axes[k]) * factors[k]);
    }
    scaled.position = point + DVec3(moved);
    return scaled;
}

bool Gizmo::update(const View& view, ImDrawList* draw, Tool tool, Space space, const Snap& snap, flecs::entity entity,
                   std::span<const flecs::entity_t> others, const DVec3& origin, bool mouse_free,
                   CommandStack& commands) {
    if (tool == Tool::Select || !entity.is_valid() || !entity.is_alive() || !entity.has<world::Transform>()) {
        handle_ = -1;
        return false;
    }
    const auto* world_now = entity.try_get<world::WorldTransform>();
    if (world_now == nullptr) {
        return false;
    }
    if (handle_ >= 0 && entity.id() != entity_) {
        handle_ = -1; // the selection changed under the drag
        commands.seal();
    }
    // While dragging, the handles stay where the drag began (moving, they go with the entity).
    DVec3 at = origin;
    if (handle_ >= 0) {
        const DVec3 gone = preview_ == Preview::Showing ? DVec3(moved_) : world_now->position - start_.position;
        at = tool == Tool::Move ? start_origin_ + gone : start_origin_;
    }
    ImVec2 center;
    if (!view.project(at, center)) {
        return handle_ >= 0;
    }
    const float meters = view.meters_per_pixel(at);
    const float length_m = px(kLengthPx) * meters;
    // The axes: the world's, or the entity's own (scaling always goes along its own); fixed while dragging.
    Vec3 axes[3] = {Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f)};
    if (handle_ >= 0) {
        std::ranges::copy(axes_, std::begin(axes));
    } else if (space == Space::Local || tool == Tool::Scale) {
        for (Vec3& axis : axes) {
            axis = glm::normalize(world_now->rotation * axis);
        }
    }
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const Vec3 eye = Vec3(view.position - at); // the camera, from the handles
    const Vec3 toward_eye = glm::normalize(eye);
    const Vec3 ray = view.ray(mouse);
    const float grab = px(kGrabPx);
    const Painter painter{
        .view = view, .draw = draw, .light = glm::normalize(view.rotation * Vec3(-0.35f, 0.65f, 0.68f))};

    // Where each handle is on screen.
    ImVec2 tips[3];      // an axis's end: past its cone, or its cube's centre
    bool shown[3] = {};  // an axis seen end on is left out
    ImVec2 planes[3][4]; // moving: a plane's corner square; scaling: its band
    bool plane_shown[3] = {};
    // Scaling has the moving handles' layout: a cube where the cone is.
    const float cube_m = length_m + (px(kConeLengthPx) * 0.5f * meters);
    const float tip_m = tool == Tool::Move ? length_m + (px(kConeLengthPx) * meters) : cube_m;
    for (int i = 0; i < 3; ++i) {
        shown[i] = view.project(at + DVec3(axes[i] * tip_m), tips[i]);
        if (shown[i]) {
            const float dx = tips[i].x - center.x;
            const float dy = tips[i].y - center.y;
            shown[i] = std::sqrt((dx * dx) + (dy * dy)) > px(kLengthPx) * 0.18f;
        }
        const Vec3& a = axes[(i + 1) % 3];
        const Vec3& b = axes[(i + 2) % 3];
        DVec3 corners[4];
        if (tool == Tool::Move) {
            const float s = length_m * kCornerShare;
            corners[0] = at;
            corners[1] = at + DVec3(a * s);
            corners[2] = at + DVec3((a + b) * s);
            corners[3] = at + DVec3(b * s);
        } else {
            // A triangle across the corner (its long side's middle as a fourth point).
            const float s = length_m * kScaleCornerShare;
            corners[0] = at;
            corners[1] = at + DVec3(a * s);
            corners[2] = at + DVec3((a + b) * (s * 0.5f));
            corners[3] = at + DVec3(b * s);
        }
        plane_shown[i] = tool != Tool::Rotate;
        for (int c = 0; c < 4 && plane_shown[i]; ++c) {
            plane_shown[i] = view.project(corners[c], planes[i][c]);
        }
        // Seen edge on, a plane's handle is a sliver: left out.
        plane_shown[i] = plane_shown[i] && std::abs(signed_area(planes[i])) > px(kLengthPx) * px(kLengthPx) * 0.004f;
    }

    // Turning: a ring round each axis, on a sphere about the middle. The halves facing the camera are the handles:
    // bands that thin as they turn away, into the far halves' lines.
    const float ring_m = px(kRingPx) * meters;
    const float band_out_m = ring_m + (px(kBandPx) * 0.5f * meters); // a band's outer edge, about
    struct RingPoint {
        ImVec2 at;
        Vec3 direction{0.0f};
        float front = 0.0f; // 1 facing the camera, 0 round the far side
    };
    RingPoint rings[3][kRingSteps];
    bool ring_shown[3] = {};
    for (int i = 0; i < 3 && tool == Tool::Rotate; ++i) {
        const Vec3& u = axes[(i + 1) % 3];
        const Vec3& w = axes[(i + 2) % 3];
        // How squarely the ring faces the camera, and how far round it its points face it at most.
        const float face_on = std::abs(glm::dot(axes[i], toward_eye));
        const float spread = std::sqrt(std::max(1.0f - (face_on * face_on), 1e-6f));
        ring_shown[i] = true;
        for (int s = 0; s < kRingSteps && ring_shown[i]; ++s) {
            const float t = 2.0f * std::numbers::pi_v<float> * static_cast<float>(s) / kRingSteps;
            RingPoint& point = rings[i][s];
            point.direction = (u * std::cos(t)) + (w * std::sin(t));
            // Thin where the ring turns away (the ends of a ring seen edge on, so the band never curls round them),
            // full facing the camera; a ring seen face on is all near: it's the sphere's outline.
            const float around = glm::dot(point.direction, toward_eye) / spread;
            point.front = std::lerp(smooth_step(-0.05f, 0.45f, around), 1.0f, smooth_step(0.8f, 0.97f, face_on));
            ring_shown[i] = view.project(at + DVec3(point.direction * ring_m), point.at);
        }
    }

    // The sphere just inside the rings: a handle too, turning the selection any way, as a ball rolled under the mouse.
    const float across = std::min(ring_m / std::max(glm::length(eye), 1e-4f), 0.95f);
    const float sphere_px = (px(kRingPx) - (px(kBandPx) * 0.5f) - px(3.0f)) / std::sqrt(1.0f - (across * across));

    // Which handle the mouse is on: the middle first, then a plane, then the nearest axis or ring.
    int hovered = -1;
    Vec3 hovered_grab(0.0f);
    if (handle_ < 0 && mouse_free) {
        if (tool == Tool::Rotate) {
            // The near halves only: the far side isn't grabbed through the sphere.
            float nearest = (px(kBandPx) * 0.5f) + px(3.0f);
            for (int i = 0; i < 3; ++i) {
                for (int s = 0; s < kRingSteps && ring_shown[i]; ++s) {
                    const RingPoint& a = rings[i][s];
                    const RingPoint& b = rings[i][(s + 1) % kRingSteps];
                    if (a.front < 0.5f || b.front < 0.5f) {
                        continue;
                    }
                    float along = 0.0f;
                    if (const float d = distance_to_segment(mouse, a.at, b.at, &along); d < nearest) {
                        nearest = d;
                        hovered = i;
                        hovered_grab = glm::normalize(a.direction + ((b.direction - a.direction) * along));
                    }
                }
            }
            // Off the rings, inside the sphere: rolling it.
            const float dx = mouse.x - center.x;
            const float dy = mouse.y - center.y;
            if (hovered < 0 && std::sqrt((dx * dx) + (dy * dy)) < sphere_px - px(2.0f)) {
                hovered = kMiddle;
            }
        } else {
            const float dx = mouse.x - center.x;
            const float dy = mouse.y - center.y;
            if (std::sqrt((dx * dx) + (dy * dy)) < px(kMiddlePx) + px(3.0f)) {
                hovered = kMiddle;
            }
            float nearest = grab;
            for (int i = 0; i < 3 && hovered != kMiddle; ++i) {
                if (shown[i]) {
                    if (const float d = distance_to_segment(mouse, center, tips[i]); d < nearest) {
                        nearest = d;
                        hovered = i;
                    }
                }
            }
            for (int i = 0; i < 3 && hovered < 0; ++i) {
                if (plane_shown[i] && inside_convex(mouse, planes[i])) {
                    hovered = kPlane + i;
                }
            }
        }
    }

    hovered_ = hovered;

    // A picture's drag (show_drag): started as a click on its handle would start it, a ring grabbed where it comes
    // nearest the camera.
    const bool asked = preview_ == Preview::Asked && handle_ < 0 && preview_handle_ >= 0 &&
                       preview_handle_ <= kMiddle &&
                       (tool != Tool::Rotate || preview_handle_ < kPlane || preview_handle_ == kMiddle);
    if (asked) {
        hovered = preview_handle_;
        if (tool == Tool::Rotate && hovered < kPlane) {
            const Vec3& axis = axes[hovered];
            const Vec3 toward = toward_eye - (axis * glm::dot(toward_eye, axis));
            hovered_grab = glm::length(toward) > 1e-3f ? glm::normalize(toward) : axes[(hovered + 1) % 3];
        }
    }

    // A drag starts on a handle.
    const Vec3 from_start = Vec3(view.position - (handle_ >= 0 ? start_origin_ : at)); // the camera, from the origin
    if (asked || (hovered >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))) {
        handle_ = hovered;
        entity_ = entity.id();
        start_ = *world_now;
        // The rest of the selection, as it begins: not what hangs on something dragged too (it follows that).
        others_.clear();
        const flecs::world ecs = entity.world();
        const auto dragged = [&](flecs::entity_t id) {
            return id == entity.id() || std::ranges::find(others, id) != others.end();
        };
        for (const flecs::entity_t id : others) {
            if (id == entity.id() || !ecs.is_alive(id)) {
                continue;
            }
            const flecs::entity other = ecs.entity(id);
            const auto* other_now = other.try_get<world::WorldTransform>();
            bool follows = false;
            for (const auto* parent = other.try_get<flecs::Parent>(); parent != nullptr && !follows;) {
                follows = dragged(parent->value);
                const flecs::entity up = ecs.entity(parent->value);
                parent = up.is_alive() ? up.try_get<flecs::Parent>() : nullptr;
            }
            if (other_now != nullptr && !follows) {
                others_.push({.entity = id, .start = *other_now});
            }
        }
        start_origin_ = at;
        std::ranges::copy(axes, std::begin(axes_));
        start_mouse_ = mouse;
        merge_key_ = core::mix64(entity.id() ^ core::mix64(++drags_)) | 1U;
        start_along_ = 0.0f;
        angle_rad_ = 0.0f;
        moved_ = Vec3(0.0f);
        scaled_ = Vec3(1.0f);
        if (tool == Tool::Rotate && hovered < kPlane) {
            // The way along the ring at the grab, on screen: dragging that way turns it forward.
            grab_ = hovered_grab;
            const Vec3 forward = glm::cross(axes[hovered], grab_);
            ImVec2 a;
            ImVec2 b;
            tangent_ = ImVec2(1.0f, 0.0f);
            if (view.project(at + DVec3(grab_ * ring_m), a) &&
                view.project(at + DVec3((grab_ * ring_m) + (forward * (ring_m * 0.05f))), b)) {
                const float dx = b.x - a.x;
                const float dy = b.y - a.y;
                if (const float length = std::sqrt((dx * dx) + (dy * dy)); length > 1e-3f) {
                    tangent_ = ImVec2(dx / length, dy / length);
                }
            }
        } else if (hovered < kPlane) {
            static_cast<void>(closest_on_axis(from_start, ray, axes[hovered], start_along_));
        } else if (tool == Tool::Move) {
            const Vec3 normal =
                hovered == kMiddle ? glm::normalize(view.rotation * Vec3(0.0f, 0.0f, -1.0f)) : axes[hovered - kPlane];
            if (!hit_plane(from_start, ray, normal, start_hit_)) {
                start_hit_ = Vec3(0.0f);
            }
        } else if (hovered != kMiddle) {
            const Vec3 diagonal = glm::normalize(axes[(hovered - kPlane + 1) % 3] + axes[(hovered - kPlane + 2) % 3]);
            static_cast<void>(closest_on_axis(from_start, ray, diagonal, start_along_));
        }
    }

    if (asked) {
        preview_ = Preview::Showing;
    }

    // The drag: the entity follows the mouse, from where it was when the drag began. A picture's is held as far as it
    // was asked to go, and changes nothing.
    if (preview_ == Preview::Showing && handle_ >= 0) {
        const float amount = preview_amount_;
        if (tool == Tool::Move) {
            const int plane = handle_ - kPlane;
            moved_ = handle_ < kPlane    ? axes_[handle_] * amount
                     : handle_ < kMiddle ? (axes_[(plane + 1) % 3] + (axes_[(plane + 2) % 3] * 0.5f)) * amount
                                         : (axes_[0] + (axes_[1] * 0.5f)) * amount;
        } else if (tool == Tool::Rotate) {
            angle_rad_ = amount;
        } else {
            for (int k = 0; k < 3; ++k) {
                const bool along = handle_ == kMiddle || (handle_ < kPlane ? k == handle_ : k != handle_ - kPlane);
                scaled_[k] = along ? 1.0f + amount : 1.0f;
            }
        }
    } else if (handle_ >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const Vec3 camera = Vec3(view.position - start_origin_);
        world::WorldTransform moved = start_;
        bool changed = true;
        // What the drag does, for the rest of the selection.
        DVec3 shift(0.0);
        Quat turn(1.0f, 0.0f, 0.0f, 0.0f);
        Vec3 stretch(1.0f);
        if (tool == Tool::Move) {
            Vec3 delta(0.0f);
            bool in_plane[3] = {};
            if (handle_ < kPlane) {
                float along = start_along_;
                changed = closest_on_axis(camera, ray, axes_[handle_], along);
                delta = axes_[handle_] * (along - start_along_);
                in_plane[handle_] = true;
            } else {
                const Vec3 normal = handle_ == kMiddle ? glm::normalize(view.rotation * Vec3(0.0f, 0.0f, -1.0f))
                                                       : axes_[handle_ - kPlane];
                Vec3 hit;
                changed = hit_plane(camera, ray, normal, hit);
                delta = hit - start_hit_;
                for (int k = 0; k < 3; ++k) {
                    in_plane[k] = handle_ == kMiddle || k != handle_ - kPlane;
                }
            }
            if (snap.move) {
                // World axes: the coordinates themselves snap to the grid; the entity's own: the distance moved.
                Vec3 rounded(0.0f);
                for (int k = 0; k < 3; ++k) {
                    if (!in_plane[k]) {
                        continue;
                    }
                    Vec3 along_axis = axes_[k];
                    if (handle_ == kMiddle) {
                        along_axis = Vec3(0.0f);
                        along_axis[k] = 1.0f; // the world's: the screen's plane has no axes of its own
                    }
                    const float distance = glm::dot(delta, along_axis);
                    if (space == Space::World || handle_ == kMiddle) {
                        const auto from = static_cast<float>(start_.position[k]);
                        rounded += along_axis * (snapped(from + distance, true, snap.move_m) - from);
                    } else {
                        rounded += along_axis * snapped(distance, true, snap.move_m);
                    }
                }
                delta = rounded;
            }
            moved.position = start_.position + DVec3(delta);
            moved_ = delta;
            shift = DVec3(delta);
        } else if (tool == Tool::Rotate) {
            const float step = snap.rotate_deg * std::numbers::pi_v<float> / 180.0f;
            if (handle_ == kMiddle) {
                // Rolled as a ball: its front goes the way the mouse does, about the axis across the drag on screen, a
                // radian for each radius of the sphere dragged.
                const float dx = mouse.x - start_mouse_.x;
                const float dy = mouse.y - start_mouse_.y;
                const Vec3 roll =
                    ((view.rotation * Vec3(0.0f, 1.0f, 0.0f)) * dx) + ((view.rotation * Vec3(1.0f, 0.0f, 0.0f)) * dy);
                const float pixels = glm::length(roll);
                angle_rad_ = snapped(pixels / std::max(sphere_px, 1.0f), snap.rotate, step);
                if (pixels > 1e-3f) {
                    turn = glm::angleAxis(angle_rad_, roll / pixels);
                    moved = turned_about(start_, start_origin_, turn);
                }
            } else {
                // Along the ring at the grab, a pixel a pixel of arc; snapping lands its turn about the axis on a step.
                const float pixels =
                    ((mouse.x - start_mouse_.x) * tangent_.x) + ((mouse.y - start_mouse_.y) * tangent_.y);
                const float angle = pixels / std::max(px(kRingPx), 1.0f);
                angle_rad_ = snap.rotate ? snapped_turn(start_.rotation, axes_[handle_], angle, step) : angle;
                turn = glm::angleAxis(angle_rad_, axes_[handle_]);
                moved = turned_about(start_, start_origin_, turn);
            }
        } else {
            Vec3 factors(1.0f);
            if (handle_ == kMiddle) {
                const float factor =
                    std::max(0.01f, 1.0f + (((mouse.x - start_mouse_.x) - (mouse.y - start_mouse_.y)) * 0.01f));
                factors = Vec3(factor);
            } else {
                const Vec3 direction =
                    handle_ < kPlane
                        ? axes_[handle_]
                        : glm::normalize(axes_[(handle_ - kPlane + 1) % 3] + axes_[(handle_ - kPlane + 2) % 3]);
                float along = start_along_;
                changed = std::abs(start_along_) > 1e-4f && closest_on_axis(camera, ray, direction, along);
                const float factor = changed ? along / start_along_ : 1.0f;
                for (int k = 0; k < 3; ++k) {
                    if (handle_ < kPlane ? k == handle_ : k != handle_ - kPlane) {
                        factors[k] = factor;
                    }
                }
            }
            // Snapped as the scale it gives, and never through zero.
            for (int k = 0; k < 3; ++k) {
                const float from = start_.scale[k];
                const float to =
                    std::max(snapped(from * factors[k], snap.scale && factors[k] != 1.0f, snap.scale_step), 0.001f);
                factors[k] = std::abs(from) > 1e-6f ? to / from : 1.0f;
            }
            scaled_ = factors;
            stretch = factors;
            moved = scaled_about(start_, start_origin_, axes_, factors);
        }
        if (changed) {
            // Back to the Transform: in its parent's space if it has one.
            world::Transform local{.position = moved.position, .rotation = moved.rotation, .scale = moved.scale};
            if (const world::WorldTransform* parent = parent_world(entity)) {
                local = world::local_of(*parent, moved);
            }
            const flecs::id_t transform = entity.world().component<world::Transform>().id();
            static_cast<void>(
                commands.set_component(entity, transform, std::as_bytes(std::span(&local, 1)), merge_key_));
            // The rest of the selection the same way, in the same command.
            for (const Other& other : others_) {
                const flecs::entity follower = entity.world().entity(other.entity);
                if (!follower.is_alive()) {
                    continue;
                }
                world::WorldTransform then = other.start;
                if (tool == Tool::Move) {
                    then.position = other.start.position + shift;
                } else if (tool == Tool::Rotate) {
                    then = turned_about(other.start, start_origin_, turn);
                } else {
                    then = scaled_about(other.start, start_origin_, axes_, stretch);
                }
                world::Transform its{.position = then.position, .rotation = then.rotation, .scale = then.scale};
                if (const world::WorldTransform* parent = parent_world(follower)) {
                    its = world::local_of(*parent, then);
                }
                static_cast<void>(
                    commands.set_component(follower, transform, std::as_bytes(std::span(&its, 1)), merge_key_));
            }
        }
    } else if (handle_ >= 0) {
        handle_ = -1; // released: the drag is one command
        commands.seal();
    }

    // The handles: what's under the mouse or dragged in yellow.
    const int lit = handle_ >= 0 ? handle_ : hovered;
    const float line = std::max(1.0f, px(kShaftPx));
    const auto ink = [&](int handle) { return lit == handle ? kHot : kAxisInk[handle % 3]; };
    // Far axes first, so the near ones cover them.
    int order[3] = {0, 1, 2};
    std::ranges::sort(order,
                      [&](int a, int b) { return glm::dot(axes[a], toward_eye) < glm::dot(axes[b], toward_eye); });

    if (tool == Tool::Rotate) {
        // The sphere just inside the rings, a handle to roll: its outline light on a dark edge, yellow under the mouse
        // or while it's rolled.
        const bool rolling = lit == kMiddle;
        draw->AddCircleFilled(center, sphere_px, rolling ? faded(kHot, 0.16f) : IM_COL32(255, 255, 255, 26),
                              kRingSteps);
        draw->AddCircle(center, sphere_px, faded(kOutline, 0.6f), kRingSteps, std::max(2.0f, px(2.5f)));
        draw->AddCircle(center, sphere_px, rolling ? kHot : IM_COL32(255, 255, 255, 170), kRingSteps,
                        std::max(1.0f, px(1.5f)));
        // The far halves: thin, faint lines, the whole ring round.
        for (int i = 0; i < 3; ++i) {
            if (!ring_shown[i]) {
                continue;
            }
            ImVec2 line_points[kRingSteps];
            for (int s = 0; s < kRingSteps; ++s) {
                line_points[s] = rings[i][s].at;
            }
            draw->AddPolyline(line_points, kRingSteps, IM_COL32(10, 10, 12, 80), ImDrawFlags_Closed,
                              std::max(1.0f, px(kBackPx)) + std::max(1.5f, px(1.5f)));
            draw->AddPolyline(line_points, kRingSteps, faded(lit == i ? kHot : kRingInk[i], 0.6f), ImDrawFlags_Closed,
                              std::max(1.0f, px(kBackPx)));
        }
        // The near halves: bands, widest facing the camera, thinning into the far halves' lines where they turn
        // away, with a dark rim. The ring seen most face on goes under the others, the one under the mouse or
        // dragged over them all.
        int drawn[3] = {0, 1, 2};
        std::ranges::sort(drawn, [&](int a, int b) {
            return std::abs(glm::dot(axes[a], toward_eye)) > std::abs(glm::dot(axes[b], toward_eye));
        });
        if (lit >= 0) {
            std::ranges::stable_partition(drawn, [&](int i) { return i != lit; });
        }
        const float band = px(kBandPx);
        const float thinnest = std::max(0.5f, px(kBackPx) * 0.5f);
        for (const int i : drawn) {
            if (!ring_shown[i]) {
                continue;
            }
            const RingPoint* ring = rings[i];
            // The run of near points, from the far point before it to the far point after; the whole ring if it's
            // all near.
            int first = -1;
            for (int s = 0; s < kRingSteps && first < 0; ++s) {
                if (ring[s].front > 0.01f && ring[(s + kRingSteps - 1) % kRingSteps].front <= 0.01f) {
                    first = s;
                }
            }
            const bool whole = first < 0 && ring[0].front > 0.01f;
            if (first < 0 && !whole) {
                continue;
            }
            int count = kRingSteps;
            if (whole) {
                first = 0;
            } else {
                count = 0;
                while (count < kRingSteps && ring[(first + count) % kRingSteps].front > 0.01f) {
                    ++count;
                }
                first = (first + kRingSteps - 1) % kRingSteps;
                count += 2;
            }
            ImVec2 left[kRingSteps + 2];
            ImVec2 right[kRingSteps + 2];
            for (int k = 0; k < count; ++k) {
                const int s = (first + k) % kRingSteps;
                const ImVec2 n = normal_of(ring[(s + kRingSteps - 1) % kRingSteps].at, ring[(s + 1) % kRingSteps].at);
                const float h = thinnest + (((band * 0.5f) - thinnest) * ring[s].front);
                left[k] = ImVec2(ring[s].at.x + (n.x * h), ring[s].at.y + (n.y * h));
                right[k] = ImVec2(ring[s].at.x - (n.x * h), ring[s].at.y - (n.y * h));
            }
            const ImU32 color = lit == i ? kHot : kRingInk[i];
            painter.faces_begin();
            for (int k = 0; k < (whole ? count : count - 1); ++k) {
                const int next = (k + 1) % count;
                ImVec2 quad[4] = {left[k], left[next], right[next], right[k]};
                fill_convex(draw, quad, color);
            }
            painter.faces_end();
            const ImDrawFlags closed = whole ? ImDrawFlags_Closed : ImDrawFlags_None;
            draw->AddPolyline(left, count, kOutline, closed, std::max(1.0f, px(1.25f)));
            draw->AddPolyline(right, count, kOutline, closed, std::max(1.0f, px(1.25f)));
        }

        if (handle_ == kMiddle) {
            // Rolling: how far, over the sphere.
            char text[32];
            const float degrees = angle_rad_ * 180.0f / std::numbers::pi_v<float>;
            static_cast<void>(std::snprintf(text, sizeof(text), "%.1f\xc2\xb0", static_cast<double>(degrees)));
            label(draw, ImVec2(center.x, center.y - sphere_px - px(18.0f)), text);
        } else if (handle_ >= 0) {
            // The turn so far, on the dragged ring in its band's style: the wedge it sweeps, a bright band along the
            // ring over it, a ruler round the ring from the grab, the ways to drag at the grab, and the angle turned.
            const Vec3& axis = axes_[handle_];
            const Vec3 side = glm::cross(axis, grab_);
            const auto on_ring = [&](float angle, float radius_m) {
                return at + DVec3(((grab_ * std::cos(angle)) + (side * std::sin(angle))) * radius_m);
            };
            const float thin = std::max(1.0f, px(1.5f));
            const float turned = std::abs(angle_rad_);
            const int steps = std::clamp(
                static_cast<int>(std::ceil(turned / (2.0f * std::numbers::pi_v<float>)*kRingSteps)), 1, kRingSteps);
            ImVec2 sweep[kRingSteps + 1];
            bool swept = turned > 1e-4f;
            for (int s = 0; s <= steps && swept; ++s) {
                swept = view.project(on_ring(angle_rad_ * static_cast<float>(s) / static_cast<float>(steps), ring_m),
                                     sweep[s]);
            }
            if (swept) {
                painter.faces_begin();
                for (int s = 0; s < steps; ++s) {
                    ImVec2 slice[3] = {center, sweep[s], sweep[s + 1]};
                    fill_convex(draw, slice, faded(kHot, 0.22f));
                }
                painter.faces_end();
                draw->AddPolyline(sweep, steps + 1, kOutline, ImDrawFlags_None, (band * 0.4f) + outline_extra());
                draw->AddPolyline(sweep, steps + 1, kSweepInk, ImDrawFlags_None, band * 0.4f);
            }
            ImVec2 edge;
            if (view.project(on_ring(0.0f, ring_m), edge)) {
                outlined_line(draw, center, edge, faded(kHot, 0.8f), thin);
            }
            if (swept && view.project(on_ring(angle_rad_, ring_m), edge)) {
                outlined_line(draw, center, edge, kHot, thin);
            }
            // The ruler: a tick each 5 degrees from the grab, longer each 15, longest each 45 with its angle; those the
            // turn has passed yellow, those round the far side faint.
            const float face_on = std::abs(glm::dot(axis, toward_eye));
            const float spread = std::sqrt(std::max(1.0f - (face_on * face_on), 1e-6f));
            const float all_near = smooth_step(0.8f, 0.97f, face_on);
            const float ruler_m = band_out_m + (px(1.0f) * meters);
            const float turned_deg = turned * 180.0f / std::numbers::pi_v<float>;
            for (int k = 0; k < 72; ++k) {
                const int deg = k * 5;
                const float t = static_cast<float>(deg) * std::numbers::pi_v<float> / 180.0f;
                const Vec3 direction = (grab_ * std::cos(t)) + (side * std::sin(t));
                const float nearness =
                    std::lerp(smooth_step(-0.05f, 0.45f, glm::dot(direction, toward_eye) / spread), 1.0f, all_near);
                const float alpha = 0.3f + (0.7f * nearness);
                // How far round the way it's turning: passed if the turn has got that far.
                const int way_deg = angle_rad_ >= 0.0f ? deg : (360 - deg) % 360;
                const bool passed = way_deg > 0 && static_cast<float>(way_deg) <= turned_deg + 0.01f;
                const ImU32 tick_ink = faded(passed ? kHot : kSweepInk, alpha);
                const float tick_px = deg % 45 == 0 ? 13.0f : (deg % 15 == 0 ? 8.0f : 4.5f);
                ImVec2 a;
                ImVec2 b;
                if (view.project(on_ring(t, ruler_m), a) &&
                    view.project(on_ring(t, ruler_m + (px(tick_px) * meters)), b)) {
                    draw->AddLine(a, b, faded(kOutline, alpha), thin + outline_extra());
                    draw->AddLine(a, b, tick_ink, thin);
                }
                ImVec2 number_at;
                if (deg % 45 == 0 && nearness > 0.5f &&
                    view.project(on_ring(t, ruler_m + (px(tick_px + 13.0f) * meters)), number_at)) {
                    char number[16];
                    static_cast<void>(
                        std::snprintf(number, sizeof(number), "%d\xc2\xb0", deg <= 180 ? deg : deg - 360));
                    const ImVec2 size = ImGui::CalcTextSize(number);
                    const ImVec2 corner(std::floor(number_at.x - (size.x * 0.5f)),
                                        std::floor(number_at.y - (size.y * 0.5f)));
                    draw->AddText(ImVec2(corner.x + 1.0f, corner.y + 1.0f), faded(kOutline, alpha), number);
                    draw->AddText(corner, tick_ink, number);
                }
            }
            // Two arrowheads at the grab, along the ring: the ways the drag turns it.
            ImVec2 grab_at;
            if (view.project(on_ring(0.0f, ring_m), grab_at)) {
                const float head = px(9.0f);
                const float wide = px(6.0f);
                const float gap = px(12.0f);
                for (const float way : {-1.0f, 1.0f}) {
                    const ImVec2 n(tangent_.x * way, tangent_.y * way);
                    const ImVec2 base(grab_at.x + (n.x * gap), grab_at.y + (n.y * gap));
                    ImVec2 arrow[3] = {ImVec2(base.x + (n.x * head), base.y + (n.y * head)),
                                       ImVec2(base.x - (n.y * wide), base.y + (n.x * wide)),
                                       ImVec2(base.x + (n.y * wide), base.y - (n.x * wide))};
                    fill_convex(draw, arrow, kSweepInk);
                    draw->AddPolyline(arrow, 3, kOutline, ImDrawFlags_Closed, std::max(1.0f, px(1.25f)));
                }
            }
            // The angle, inside the wedge it sweeps (clear of the ruler's numbers).
            char text[32];
            const float degrees = angle_rad_ * 180.0f / std::numbers::pi_v<float>;
            static_cast<void>(std::snprintf(text, sizeof(text), "%.1f\xc2\xb0", static_cast<double>(degrees)));
            ImVec2 tag;
            if (view.project(on_ring(angle_rad_ * 0.5f, ring_m * 0.62f), tag)) {
                label(draw, tag, text);
            }
        }
        // The middle, as the move handles have it: what the turning goes about.
        painter.sphere(center, px(kMiddlePx) * 0.8f, kMiddleInk);
        return hovered >= 0 || handle_ >= 0;
    }

    // Moving and scaling. While dragging, only what the drag goes along shows: its axis, or its plane and that plane's
    // two axes; scaling whole, every axis.
    const auto showing = [&](int handle) {
        if (handle_ < 0 || handle == handle_) {
            return true;
        }
        if (handle_ >= kPlane && handle_ < kMiddle) {
            return handle < kPlane && handle != handle_ - kPlane;
        }
        return handle_ == kMiddle && tool == Tool::Scale && handle < kPlane;
    };
    // The planes' handles, under the axes.
    const float under = line + outline_extra();
    for (int i = 0; i < 3; ++i) {
        if (!plane_shown[i] || !showing(kPlane + i)) {
            continue;
        }
        const int a = (i + 1) % 3;
        const int b = (i + 2) % 3;
        const bool hot = lit == kPlane + i;
        if (tool == Tool::Move) {
            // A corner: from each axis toward the other, in that axis's colour.
            if (hot) {
                ImVec2 square[4] = {planes[i][0], planes[i][1], planes[i][2], planes[i][3]};
                fill_convex(draw, square, faded(kHot, 0.3f));
            }
            draw->AddLine(planes[i][1], planes[i][2], kOutline, under);
            draw->AddLine(planes[i][3], planes[i][2], kOutline, under);
            draw->AddLine(planes[i][1], planes[i][2], hot ? kHot : kAxisInk[a], line);
            draw->AddLine(planes[i][3], planes[i][2], hot ? kHot : kAxisInk[b], line);
        } else {
            // A line across the corner, half in each axis's colour.
            if (hot) {
                ImVec2 triangle[3] = {planes[i][0], planes[i][1], planes[i][3]};
                fill_convex(draw, triangle, faded(kHot, 0.3f));
            }
            draw->AddLine(planes[i][1], planes[i][2], kOutline, under);
            draw->AddLine(planes[i][2], planes[i][3], kOutline, under);
            draw->AddLine(planes[i][1], planes[i][2], hot ? kHot : kAxisInk[a], line);
            draw->AddLine(planes[i][2], planes[i][3], hot ? kHot : kAxisInk[b], line);
        }
    }
    // The axes, far ones first.
    const float middle_m = px(kMiddlePx) * meters;
    for (const int i : order) {
        if (!shown[i] || !showing(i)) {
            continue;
        }
        const ImU32 color = lit == kMiddle && tool == Tool::Scale ? kHot : ink(i);
        // Scaling under way, the cube goes out or in with its axis's scale.
        const float stretch = tool == Tool::Scale && handle_ >= 0 ? scaled_[i] : 1.0f;
        ImVec2 from;
        ImVec2 to;
        const float end_m =
            tool == Tool::Move ? length_m : std::max((cube_m * stretch) - (px(kCubePx) * meters), middle_m);
        if (view.project(at + DVec3(axes[i] * middle_m), from) && view.project(at + DVec3(axes[i] * end_m), to)) {
            outlined_line(draw, from, to, color, line);
        }
        if (tool == Tool::Move) {
            painter.cone(at + DVec3(axes[i] * length_m), axes[i], px(kConeLengthPx) * meters,
                         px(kConeRadiusPx) * meters, color);
        } else {
            ImVec2 was;
            if (stretch != 1.0f && view.project(at + DVec3(axes[i] * cube_m), was)) {
                start_marker(draw, was);
            }
            painter.cube(at + DVec3(axes[i] * (cube_m * stretch)), axes, px(kCubePx) * meters, color);
        }
    }
    // A move under way: where it began (a ring), a dashed line from there to where it is, and how far in centimetres
    // beside the line: along the axis dragged, or straight across.
    if (tool == Tool::Move && handle_ >= 0) {
        ImVec2 tag(center.x, center.y - px(28.0f));
        ImVec2 began;
        if (view.project(start_origin_, began)) {
            dashed_line(draw, began, center, kHot, std::max(1.0f, px(2.0f)));
            start_marker(draw, began);
            // Halfway along, off to the side above it.
            ImVec2 side = normal_of(began, center);
            if (side.y > 0.0f) {
                side = ImVec2(-side.x, -side.y);
            }
            if (side.x != 0.0f || side.y != 0.0f) {
                tag = ImVec2(((began.x + center.x) * 0.5f) + (side.x * px(20.0f)),
                             ((began.y + center.y) * 0.5f) + (side.y * px(20.0f)));
            }
        }
        char text[48];
        const float cm = (handle_ < kPlane ? glm::dot(moved_, axes_[handle_]) : glm::length(moved_)) * 100.0f;
        static_cast<void>(
            std::snprintf(text, sizeof(text), handle_ < kPlane ? "%+.1f cm" : "%.1f cm", static_cast<double>(cm)));
        label(draw, tag, text);
    }
    // Scaling under way: by how much, past the cube along one axis (and the scale it gives), or over the middle.
    if (tool == Tool::Scale && handle_ >= 0) {
        char text[64];
        ImVec2 tag(center.x, center.y - px(30.0f));
        if (handle_ < kPlane) {
            static_cast<void>(std::snprintf(text, sizeof(text), "\xc3\x97%.2f  (%.2f)",
                                            static_cast<double>(scaled_[handle_]),
                                            static_cast<double>(start_.scale[handle_] * scaled_[handle_])));
            ImVec2 beyond;
            if (view.project(at + DVec3(axes_[handle_] * ((cube_m * scaled_[handle_]) + (px(34.0f) * meters))),
                             beyond)) {
                tag = beyond;
            }
        } else {
            const int k = handle_ == kMiddle ? 0 : (handle_ - kPlane + 1) % 3;
            static_cast<void>(std::snprintf(text, sizeof(text), "\xc3\x97%.2f", static_cast<double>(scaled_[k])));
        }
        label(draw, tag, text);
    }
    // The middle: a sphere to move in the screen's plane, a cube to scale whole.
    const ImU32 middle_ink = lit == kMiddle ? kHot : kMiddleInk;
    if (tool == Tool::Move) {
        painter.sphere(center, px(kMiddlePx), middle_ink);
    } else {
        painter.cube(at, axes, middle_m, middle_ink);
    }
    return hovered >= 0 || handle_ >= 0;
}

} // namespace de::editor
