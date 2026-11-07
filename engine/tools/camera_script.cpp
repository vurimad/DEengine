#include "tools/camera_script.h"

#include "core/allocator.h"
#include "core/fixed_string.h"
#include "core/log.h"
#include "core/text.h"
#include "tools/free_camera.h"

#include <glm/trigonometric.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <numbers>
#include <system_error>

namespace de::tools {

namespace {

constexpr size_t kPoseFields = 5;

// The next word of a line, taken off its front; empty at the end of it.
std::string_view next_word(std::string_view& line) {
    line = core::trim(line);
    size_t end = 0;
    while (end < line.size() && !core::is_blank(line[end])) {
        ++end;
    }
    const std::string_view word = line.substr(0, end);
    line.remove_prefix(end);
    return word;
}

// All of a word, as a finite number. from_chars knows no locale, and a double keeps what a position needs.
bool parse_number(std::string_view word, double& out) {
    const auto [stop, error] = std::from_chars(word.data(), word.data() + word.size(), out);
    return error == std::errc{} && stop == word.data() + word.size() && std::isfinite(out);
}

// x, y, z, yaw_deg, pitch_deg, range checked.
core::Result<CameraPose> pose_from(const double (&fields)[kPoseFields]) {
    const double yaw_rad = glm::radians(fields[3]);
    const double pitch_rad = glm::radians(fields[4]);
    if (std::abs(pitch_rad) > static_cast<double>(FreeCamera::kMaxPitchRad)) {
        return core::fail(core::ErrorCode::OutOfRange, "the pitch is past what the free camera holds (88.8 degrees)");
    }
    return CameraPose{.position = {fields[0], fields[1], fields[2]},
                      .yaw_rad = static_cast<float>(yaw_rad),
                      .pitch_rad = static_cast<float>(pitch_rad)};
}

// A pose's fields from the words of a line; nothing may follow them.
core::Result<CameraPose> pose_from_words(std::string_view line) {
    double fields[kPoseFields] = {};
    for (double& field : fields) {
        if (!parse_number(next_word(line), field)) {
            return core::fail(core::ErrorCode::InvalidArgument,
                              "a pose is x y z yaw_deg pitch_deg, all of them numbers");
        }
    }
    if (!core::trim(line).empty()) {
        return core::fail(core::ErrorCode::InvalidArgument, "a pose is five numbers, and something follows them");
    }
    return pose_from(fields);
}

} // namespace

core::Result<CameraPose> parse_camera_pose(std::string_view text) {
    double fields[kPoseFields] = {};
    for (size_t i = 0; i < kPoseFields; ++i) {
        const size_t comma = text.find(',');
        const bool last = i + 1 == kPoseFields;
        if (last != (comma == std::string_view::npos)) {
            return core::fail(core::ErrorCode::InvalidArgument, "a pose has five fields: x,y,z,yaw_deg,pitch_deg");
        }
        if (!parse_number(text.substr(0, comma), fields[i])) {
            return core::fail(core::ErrorCode::InvalidArgument, "a pose's fields are numbers: x,y,z,yaw_deg,pitch_deg");
        }
        text.remove_prefix(last ? text.size() : comma + 1);
    }
    return pose_from(fields);
}

core::Result<CameraPose> find_camera_view(std::string_view text, std::string_view name, const char* source) {
    core::Array<std::string_view> names(core::heap_allocator(core::MemoryTag::Tools));
    bool found = false;
    CameraPose pose;
    uint32_t line_number = 0;
    std::string_view rest = core::skip_bom(text);
    while (!rest.empty()) {
        ++line_number;
        std::string_view line = core::strip_comment(core::next_line(rest));
        if (line.empty()) {
            continue;
        }
        const std::string_view view = next_word(line);
        const core::Result<CameraPose> parsed = pose_from_words(line);
        if (!parsed) {
            DE_LOG_ERROR("eyes", "%s:%u: view '%.*s': %s", source, line_number, static_cast<int>(view.size()),
                         view.data(), parsed.error().message);
            return core::fail(core::ErrorCode::InvalidArgument, "a view's line is a name and a pose");
        }
        if (std::ranges::find(names, view) != names.end()) {
            DE_LOG_ERROR("eyes", "%s:%u: there's already a view '%.*s'", source, line_number,
                         static_cast<int>(view.size()), view.data());
            return core::fail(core::ErrorCode::AlreadyExists, "two views have one name");
        }
        names.push(view);
        if (view == name) {
            pose = *parsed;
            found = true;
        }
    }
    if (!found) {
        core::FixedString<512> known;
        for (const std::string_view view : names) {
            static_cast<void>(known.append(' ') && known.append(view));
        }
        DE_LOG_ERROR("eyes", "%s has no view '%.*s'; it has:%s", source, static_cast<int>(name.size()), name.data(),
                     known.empty() ? " none" : known.c_str());
        return core::fail(core::ErrorCode::NotFound, "no view by that name");
    }
    return pose;
}

core::Result<CameraPath> CameraPath::parse(std::string_view text, const char* source) {
    CameraPath path;
    bool has_speed = false;
    uint32_t line_number = 0;
    std::string_view rest = core::skip_bom(text);
    while (!rest.empty()) {
        ++line_number;
        const std::string_view line = core::strip_comment(core::next_line(rest));
        if (line.empty()) {
            continue;
        }
        if (std::string_view words = line; next_word(words) == "speed") {
            double speed = 0.0;
            if (has_speed || !parse_number(next_word(words), speed) || !(speed > 0.0) || !core::trim(words).empty()) {
                DE_LOG_ERROR("eyes", "%s:%u: a path has one speed, a number of m/s over 0", source, line_number);
                return core::fail(core::ErrorCode::InvalidArgument, "a path's speed is one number over 0");
            }
            path.speed_mps_ = speed;
            has_speed = true;
            continue;
        }
        const core::Result<CameraPose> pose = pose_from_words(line);
        if (!pose) {
            DE_LOG_ERROR("eyes", "%s:%u: %s", source, line_number, pose.error().message);
            return core::fail(core::ErrorCode::InvalidArgument, "a waypoint is a pose");
        }
        Waypoint next{.position = pose->position,
                      .yaw_rad = static_cast<double>(pose->yaw_rad),
                      .pitch_rad = static_cast<double>(pose->pitch_rad)};
        if (!path.waypoints_.empty()) {
            const Waypoint& last = path.waypoints_.back();
            const double stretch = glm::distance(last.position, next.position);
            if (!(stretch > 0.0)) {
                DE_LOG_ERROR("eyes", "%s:%u: this waypoint is where the last one is: nothing flies between them",
                             source, line_number);
                return core::fail(core::ErrorCode::InvalidArgument, "two waypoints in a row at one place");
            }
            next.start_m = last.start_m + stretch;
        }
        path.waypoints_.push(next);
    }
    if (path.waypoints_.size() < 2) {
        DE_LOG_ERROR("eyes", "%s: a path needs at least two waypoints, and has %zu", source, path.waypoints_.size());
        return core::fail(core::ErrorCode::InvalidArgument, "a path needs at least two waypoints");
    }
    return path;
}

CameraPose CameraPath::at(double distance_m) const {
    const std::span<const Waypoint> points = waypoints_.span();
    const double distance = std::clamp(distance_m, 0.0, length_m());
    // The first waypoint past it, at most the last: the stretch runs up to it.
    const auto past = std::upper_bound(points.begin() + 1, points.end() - 1, distance,
                                       [](double along, const Waypoint& point) { return along < point.start_m; });
    const Waypoint& to = *past;
    const Waypoint& from = *(past - 1);
    const double t = (distance - from.start_m) / (to.start_m - from.start_m);
    const double turn = std::remainder(to.yaw_rad - from.yaw_rad, 2.0 * std::numbers::pi);
    return {.position = {std::lerp(from.position.x, to.position.x, t), std::lerp(from.position.y, to.position.y, t),
                         std::lerp(from.position.z, to.position.z, t)},
            .yaw_rad = static_cast<float>(from.yaw_rad + (turn * t)),
            .pitch_rad = static_cast<float>(std::lerp(from.pitch_rad, to.pitch_rad, t))};
}

uint64_t CameraPath::flight_frames(double step_s) const {
    // Held to what a whole number holds; a flight anywhere near that is refused long before it's flown.
    const double frames = std::min(length_m() / (speed_mps_ * step_s), 1e18);
    return static_cast<uint64_t>(std::ceil(frames)) + 1;
}

CameraPose CameraPath::at_flight_frame(uint64_t frame, double step_s) const {
    return at(static_cast<double>(frame) * step_s * speed_mps_);
}

} // namespace de::tools
