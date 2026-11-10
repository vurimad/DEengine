#include "editor/icons.h"

#include "core/fixed_string.h"
#include "core/log.h"
#include "platform/vfs.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <numbers>

namespace de::editor {

namespace {

constexpr const char* kNames[] = {
    "align_to_surface",
    "camera",
    "center",
    "character",
    "eye",
    "fast_forward",
    "focus_selection",
    "group",
    "link",
    "local_coords",
    "lock",
    "move",
    "pause",
    "pivot",
    "play",
    "redo",
    "rotate",
    "save",
    "scale",
    "select",
    "select_box",
    "snap_angle",
    "snap_grid",
    "snap_scale",
    "snap_terrain",
    "step_forward",
    "stop",
    "terrain",
    "undo",
    "ungroup",
    "unlink",
    "unlock",
    "world_coords",
    "zoom",
};
static_assert(std::size(kNames) == static_cast<size_t>(Icon::Count));

constexpr int kCurveSteps = 12;
constexpr int kCircleSteps = 32;
constexpr uint32_t kMaxShapePoints = 256; // what draw() takes of a shape

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// An attribute's value in a tag's text (` name="value"`); empty if it isn't there.
std::string_view attribute(std::string_view tag, std::string_view name) {
    for (size_t at = tag.find(name); at != std::string_view::npos; at = tag.find(name, at + 1)) {
        const size_t equals = at + name.size();
        const bool starts =
            at > 0 && (tag[at - 1] == ' ' || tag[at - 1] == '\t' || tag[at - 1] == '\n' || tag[at - 1] == '\r');
        if (starts && equals + 1 < tag.size() && tag[equals] == '=' && tag[equals + 1] == '"') {
            const size_t end = tag.find('"', equals + 2);
            if (end != std::string_view::npos) {
                return tag.substr(equals + 2, end - equals - 2);
            }
        }
    }
    return {};
}

// A path's data ("M12 3v18M3 12h18"), a command or a number at a time: numbers may run together ("1.8-.9", ".5.5").
class PathReader {
public:
    explicit PathReader(std::string_view text) : text_(text) {}

    bool at_end() {
        skip();
        return at_ >= text_.size();
    }
    bool command_next() {
        skip();
        return at_ < text_.size() && is_letter(text_[at_]);
    }
    char command() { return text_[at_++]; }

    bool number(float& out) {
        skip();
        size_t end = at_;
        if (end < text_.size() && (text_[end] == '-' || text_[end] == '+')) {
            ++end;
        }
        bool digits = false;
        while (end < text_.size() && is_digit(text_[end])) {
            ++end;
            digits = true;
        }
        if (end < text_.size() && text_[end] == '.') {
            ++end;
            while (end < text_.size() && is_digit(text_[end])) {
                ++end;
                digits = true;
            }
        }
        if (!digits) {
            return false;
        }
        if (end < text_.size() && (text_[end] == 'e' || text_[end] == 'E')) {
            size_t exponent = end + 1;
            if (exponent < text_.size() && (text_[exponent] == '-' || text_[exponent] == '+')) {
                ++exponent;
            }
            if (exponent < text_.size() && is_digit(text_[exponent])) {
                end = exponent;
                while (end < text_.size() && is_digit(text_[end])) {
                    ++end;
                }
            }
        }
        char buffer[32] = {};
        std::memcpy(buffer, text_.data() + at_, std::min(end - at_, sizeof(buffer) - 1));
        out = std::strtof(buffer, nullptr);
        at_ = end;
        return true;
    }

    // An arc's flag: one character, 0 or 1, which may run into what follows ("0 0 0-2.05").
    bool flag(bool& out) {
        skip();
        if (at_ < text_.size() && (text_[at_] == '0' || text_[at_] == '1')) {
            out = text_[at_++] == '1';
            return true;
        }
        return false;
    }

private:
    void skip() {
        while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == ',' || text_[at_] == '\t' ||
                                      text_[at_] == '\n' || text_[at_] == '\r')) {
            ++at_;
        }
    }

    std::string_view text_;
    size_t at_ = 0;
};

float number_or(std::string_view text, float fallback) {
    PathReader reader(text);
    float value = fallback;
    return reader.number(value) ? value : fallback;
}

float angle_between(Vec2 u, Vec2 v) {
    return std::atan2((u.x * v.y) - (u.y * v.x), (u.x * v.x) + (u.y * v.y));
}

} // namespace

bool IconSet::parse(Icon icon, std::string_view svg) {
    Range& range = icons_[static_cast<uint32_t>(icon)];
    range = {.first_shape = static_cast<uint32_t>(shapes_.size()), .shape_count = 0, .view_size = 24.0f};
    const size_t root_start = svg.find("<svg");
    const size_t root_end = root_start == std::string_view::npos ? root_start : svg.find('>', root_start);
    if (root_end == std::string_view::npos) {
        return false;
    }
    const std::string_view root = svg.substr(root_start, root_end - root_start);
    if (const std::string_view box = attribute(root, "viewBox"); !box.empty()) {
        PathReader reader(box);
        float numbers[4] = {0.0f, 0.0f, 24.0f, 24.0f};
        for (float& number : numbers) {
            static_cast<void>(reader.number(number));
        }
        range.view_size = numbers[2] > 0.0f ? numbers[2] : 24.0f;
    }
    // The root's fill and stroke, which an element may change.
    struct Style {
        bool fill = false;
        bool stroke = true;
        float stroke_width = 1.0f;
        float dash = 0.0f;
        float gap = 0.0f;
    };
    Style root_style;
    root_style.fill = attribute(root, "fill") == "currentColor";
    root_style.stroke = attribute(root, "stroke") != "none";
    root_style.stroke_width = number_or(attribute(root, "stroke-width"), 1.0f);

    Style style;
    uint32_t first = 0;
    const auto begin = [&] { first = static_cast<uint32_t>(points_.size()); };
    const auto point = [&](Vec2 p) { points_.push(p); };
    const auto end = [&](bool closed) {
        const auto count = static_cast<uint32_t>(points_.size()) - first;
        if (count < 2) {
            points_.resize(first);
            return;
        }
        shapes_.push({.first = first,
                      .count = count,
                      .stroke_width = style.stroke ? style.stroke_width : 0.0f,
                      .dash = style.dash,
                      .gap = style.gap,
                      .closed = closed,
                      .fill = style.fill});
    };
    const auto cubic = [&](Vec2 from, Vec2 c1, Vec2 c2, Vec2 to) {
        for (int i = 1; i <= kCurveSteps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kCurveSteps);
            const float u = 1.0f - t;
            point((from * (u * u * u)) + (c1 * (3.0f * u * u * t)) + (c2 * (3.0f * u * t * t)) + (to * (t * t * t)));
        }
    };
    // An elliptical arc from its end points, as SVG gives it, through its centre.
    const auto arc = [&](Vec2 from, float rx, float ry, float rotation_deg, bool large, bool sweep, Vec2 to) {
        if (rx == 0.0f || ry == 0.0f || from == to) {
            point(to);
            return;
        }
        rx = std::abs(rx);
        ry = std::abs(ry);
        const float phi = rotation_deg * std::numbers::pi_v<float> / 180.0f;
        const float cos_phi = std::cos(phi);
        const float sin_phi = std::sin(phi);
        const Vec2 half = (from - to) * 0.5f;
        const Vec2 p((cos_phi * half.x) + (sin_phi * half.y), (-sin_phi * half.x) + (cos_phi * half.y));
        if (const float lambda = ((p.x * p.x) / (rx * rx)) + ((p.y * p.y) / (ry * ry)); lambda > 1.0f) {
            rx *= std::sqrt(lambda);
            ry *= std::sqrt(lambda);
        }
        const float numerator = (rx * rx * ry * ry) - (rx * rx * p.y * p.y) - (ry * ry * p.x * p.x);
        const float denominator = (rx * rx * p.y * p.y) + (ry * ry * p.x * p.x);
        const float root_term = std::sqrt(std::max(0.0f, numerator / std::max(denominator, 1e-12f)));
        const float sign = large == sweep ? -1.0f : 1.0f;
        const Vec2 center_p(sign * root_term * (rx * p.y / ry), sign * root_term * -(ry * p.x / rx));
        const Vec2 middle = (from + to) * 0.5f;
        const Vec2 center((cos_phi * center_p.x) - (sin_phi * center_p.y) + middle.x,
                          (sin_phi * center_p.x) + (cos_phi * center_p.y) + middle.y);
        const Vec2 start_v((p.x - center_p.x) / rx, (p.y - center_p.y) / ry);
        const Vec2 end_v((-p.x - center_p.x) / rx, (-p.y - center_p.y) / ry);
        const float theta = angle_between(Vec2(1.0f, 0.0f), start_v);
        float delta = angle_between(start_v, end_v);
        if (!sweep && delta > 0.0f) {
            delta -= 2.0f * std::numbers::pi_v<float>;
        } else if (sweep && delta < 0.0f) {
            delta += 2.0f * std::numbers::pi_v<float>;
        }
        const int steps =
            std::max(4, static_cast<int>(std::ceil(std::abs(delta) / (std::numbers::pi_v<float> / 16.0f))));
        for (int i = 1; i <= steps; ++i) {
            const float t = theta + (delta * static_cast<float>(i) / static_cast<float>(steps));
            point(Vec2(center.x + (rx * std::cos(t) * cos_phi) - (ry * std::sin(t) * sin_phi),
                       center.y + (rx * std::cos(t) * sin_phi) + (ry * std::sin(t) * cos_phi)));
        }
        point(to);
    };

    for (size_t at = svg.find('<', root_end); at != std::string_view::npos; at = svg.find('<', at + 1)) {
        const size_t tag_end = svg.find('>', at);
        if (tag_end == std::string_view::npos) {
            break;
        }
        const std::string_view tag = svg.substr(at, tag_end - at);
        style = root_style;
        if (const std::string_view fill = attribute(tag, "fill"); !fill.empty()) {
            style.fill = fill == "currentColor";
        }
        if (const std::string_view stroke = attribute(tag, "stroke"); !stroke.empty()) {
            style.stroke = stroke != "none";
        }
        style.stroke_width = number_or(attribute(tag, "stroke-width"), style.stroke_width);
        if (const std::string_view dashes = attribute(tag, "stroke-dasharray"); !dashes.empty()) {
            PathReader reader(dashes);
            static_cast<void>(reader.number(style.dash));
            style.gap = style.dash;
            static_cast<void>(reader.number(style.gap));
        }
        if (tag.starts_with("<circle")) {
            const Vec2 center(number_or(attribute(tag, "cx"), 0.0f), number_or(attribute(tag, "cy"), 0.0f));
            const float radius = number_or(attribute(tag, "r"), 0.0f);
            begin();
            for (int i = 0; i < kCircleSteps; ++i) {
                const float t =
                    2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(kCircleSteps);
                point(center + (Vec2(std::cos(t), std::sin(t)) * radius));
            }
            end(true);
        } else if (tag.starts_with("<rect")) {
            const float x = number_or(attribute(tag, "x"), 0.0f);
            const float y = number_or(attribute(tag, "y"), 0.0f);
            const float width = number_or(attribute(tag, "width"), 0.0f);
            const float height = number_or(attribute(tag, "height"), 0.0f);
            const float radius = std::min({number_or(attribute(tag, "rx"), 0.0f), width * 0.5f, height * 0.5f});
            begin();
            // Corner by corner, clockwise from the top left: a quarter circle each, or the corner itself.
            const Vec2 corners[] = {Vec2(x + radius, y + radius), Vec2(x + width - radius, y + radius),
                                    Vec2(x + width - radius, y + height - radius),
                                    Vec2(x + radius, y + height - radius)};
            for (int corner = 0; corner < 4; ++corner) {
                const float start = std::numbers::pi_v<float> * (1.0f + (0.5f * static_cast<float>(corner)));
                const int steps = radius > 0.0f ? 6 : 0;
                for (int i = 0; i <= steps; ++i) {
                    const float t = start + (0.5f * std::numbers::pi_v<float> * static_cast<float>(i) /
                                             static_cast<float>(std::max(steps, 1)));
                    point(corners[corner] + (Vec2(std::cos(t), std::sin(t)) * radius));
                }
            }
            end(true);
        } else if (tag.starts_with("<path")) {
            PathReader reader(attribute(tag, "d"));
            Vec2 current(0.0f);
            Vec2 start(0.0f);
            Vec2 last_control(0.0f);
            char previous = 0;
            bool open = false;
            while (!reader.at_end()) {
                char command = 0;
                if (reader.command_next()) {
                    command = reader.command();
                } else if (previous != 0 && previous != 'Z' && previous != 'z') {
                    command = previous; // a command's numbers repeat it
                } else {
                    return false;
                }
                const bool relative = command >= 'a' && command <= 'z';
                const char kind = relative ? static_cast<char>(command - 'a' + 'A') : command;
                const Vec2 origin = relative ? current : Vec2(0.0f);
                if (kind != 'M' && kind != 'Z' && !open) {
                    begin();
                    point(current);
                    open = true;
                }
                float n[7] = {};
                const auto numbers = [&](int count) {
                    for (int i = 0; i < count; ++i) {
                        if (!reader.number(n[i])) {
                            return false;
                        }
                    }
                    return true;
                };
                bool smooth_control = false;
                switch (kind) {
                case 'M':
                    if (!numbers(2)) {
                        return false;
                    }
                    if (open) {
                        end(false);
                    }
                    current = start = origin + Vec2(n[0], n[1]);
                    begin();
                    point(current);
                    open = true;
                    previous = relative ? 'l' : 'L'; // what follows a move draws lines
                    last_control = current;
                    continue;
                case 'L':
                    if (!numbers(2)) {
                        return false;
                    }
                    current = origin + Vec2(n[0], n[1]);
                    point(current);
                    break;
                case 'H':
                    if (!numbers(1)) {
                        return false;
                    }
                    current.x = (relative ? current.x : 0.0f) + n[0];
                    point(current);
                    break;
                case 'V':
                    if (!numbers(1)) {
                        return false;
                    }
                    current.y = (relative ? current.y : 0.0f) + n[0];
                    point(current);
                    break;
                case 'C': {
                    if (!numbers(6)) {
                        return false;
                    }
                    const Vec2 c1 = origin + Vec2(n[0], n[1]);
                    const Vec2 c2 = origin + Vec2(n[2], n[3]);
                    const Vec2 to = origin + Vec2(n[4], n[5]);
                    cubic(current, c1, c2, to);
                    last_control = c2;
                    current = to;
                    smooth_control = true;
                    break;
                }
                case 'S': {
                    if (!numbers(4)) {
                        return false;
                    }
                    // The first control mirrors the last curve's second, if the last was a curve.
                    const bool after_curve = previous == 'C' || previous == 'c' || previous == 'S' || previous == 's';
                    const Vec2 c1 = after_curve ? (current * 2.0f) - last_control : current;
                    const Vec2 c2 = origin + Vec2(n[0], n[1]);
                    const Vec2 to = origin + Vec2(n[2], n[3]);
                    cubic(current, c1, c2, to);
                    last_control = c2;
                    current = to;
                    smooth_control = true;
                    break;
                }
                case 'A': {
                    bool large = false;
                    bool sweep = false;
                    if (!numbers(3) || !reader.flag(large) || !reader.flag(sweep) || !reader.number(n[5]) ||
                        !reader.number(n[6])) {
                        return false;
                    }
                    const Vec2 to = origin + Vec2(n[5], n[6]);
                    arc(current, n[0], n[1], n[2], large, sweep, to);
                    current = to;
                    break;
                }
                case 'Z':
                    if (open) {
                        end(true);
                        open = false;
                    }
                    current = start;
                    break;
                default:
                    return false; // not in the icons: Q, T
                }
                if (!smooth_control) {
                    last_control = current;
                }
                previous = command;
            }
            if (open) {
                end(false);
            }
        }
    }
    range.shape_count = static_cast<uint32_t>(shapes_.size()) - range.first_shape;
    return range.shape_count > 0;
}

bool IconSet::load(const platform::Vfs& vfs) {
    bool all = true;
    for (uint32_t i = 0; i < static_cast<uint32_t>(Icon::Count); ++i) {
        core::FixedString<96> path;
        static_cast<void>(path.assign("engine:/engine/editor/icons/"));
        static_cast<void>(path.append(kNames[i]));
        static_cast<void>(path.append(".svg"));
        auto file = vfs.read_whole_file(path.c_str(), core::heap_allocator(core::MemoryTag::Editor));
        const bool parsed = file && parse(static_cast<Icon>(i),
                                          std::string_view(reinterpret_cast<const char*>(file->data()), file->size()));
        if (!parsed) {
            DE_LOG_WARN("editor", "icon %s didn't load", path.c_str());
            all = false;
        }
    }
    return all;
}

void IconSet::draw(ImDrawList* draw, Icon icon, ImVec2 min, float size, ImU32 color) const {
    const Range& range = icons_[static_cast<uint32_t>(icon)];
    const float scale = size / range.view_size;
    ImVec2 screen[kMaxShapePoints];
    for (uint32_t s = 0; s < range.shape_count; ++s) {
        const Shape& shape = shapes_[range.first_shape + s];
        const uint32_t count = std::min(shape.count, kMaxShapePoints);
        for (uint32_t i = 0; i < count; ++i) {
            const Vec2 p = points_[shape.first + i];
            screen[i] = ImVec2(min.x + (p.x * scale), min.y + (p.y * scale));
        }
        if (shape.fill && count >= 3) {
            draw->AddConcavePolyFilled(screen, static_cast<int>(count), color);
        }
        if (shape.stroke_width <= 0.0f) {
            continue;
        }
        const float width = std::max(1.0f, shape.stroke_width * scale);
        if (shape.dash > 0.0f) {
            // Dashes along the outline: drawn for dash, skipped for gap, carried across corners.
            const float dash = shape.dash * scale;
            const float gap = std::max(shape.gap * scale, 0.5f);
            float phase = 0.0f; // into the current dash or gap
            bool drawing = true;
            const uint32_t segments = shape.closed ? count : count - 1;
            for (uint32_t i = 0; i < segments; ++i) {
                const ImVec2 a = screen[i];
                const ImVec2 b = screen[(i + 1) % count];
                const float length = std::sqrt(((b.x - a.x) * (b.x - a.x)) + ((b.y - a.y) * (b.y - a.y)));
                float along = 0.0f;
                while (along < length) {
                    const float left = (drawing ? dash : gap) - phase;
                    const float step = std::min(left, length - along);
                    if (drawing) {
                        const float t0 = along / length;
                        const float t1 = (along + step) / length;
                        draw->AddLine(ImVec2(a.x + ((b.x - a.x) * t0), a.y + ((b.y - a.y) * t0)),
                                      ImVec2(a.x + ((b.x - a.x) * t1), a.y + ((b.y - a.y) * t1)), color, width);
                    }
                    along += step;
                    phase += step;
                    if (phase >= (drawing ? dash : gap)) {
                        phase = 0.0f;
                        drawing = !drawing;
                    }
                }
            }
            continue;
        }
        draw->AddPolyline(screen, static_cast<int>(count), color, shape.closed ? ImDrawFlags_Closed : ImDrawFlags_None,
                          width);
        if (!shape.closed && width > 1.5f) {
            // Round caps, as the icons ask.
            draw->AddCircleFilled(screen[0], width * 0.5f, color);
            draw->AddCircleFilled(screen[count - 1], width * 0.5f, color);
        }
    }
}

} // namespace de::editor
