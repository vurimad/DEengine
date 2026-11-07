#pragma once

#include "core/array.h"
#include "core/math.h"
#include "core/result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace de::tools {

// Where a camera is and where it looks, as FreeCamera takes it. The position stays in doubles from the
// text to the entity, so a place 16 km from the origin lands where it says.
struct CameraPose {
    DVec3 position{0.0};
    float yaw_rad = 0.0f;   // about world up; 0 looks down -Z
    float pitch_rad = 0.0f; // up is positive
};

// A pose as one word: "x,y,z,yaw_deg,pitch_deg", in meters and degrees, as the stats panel shows the camera
// (F1). Every field is needed. A pitch past what the free camera holds (FreeCamera::kMaxPitchRad) fails:
// clamped, the picture wouldn't be the one asked for.
core::Result<CameraPose> parse_camera_pose(std::string_view text);

// The named views' text (config/camera.views): one a line, a name and a pose, blanks between its fields.
//     camp_south   64 2 74 0 -7
//     far_corner   16000.25 40 -16000.5 135 -5
// '#' starts a comment; Windows line ends and a byte order mark are fine. The whole text is checked, whichever
// view is asked for, and a name twice is an error. A bad line or a missing name fails, logged with source's
// name and the line, or the names there are.
core::Result<CameraPose> find_camera_view(std::string_view text, std::string_view name, const char* source);

// What a horse gallops at, and so what a camera path flies at unless it says otherwise.
inline constexpr double kGallopMps = 13.0;

// Waypoints flown in straight lines at one speed, turning evenly along each stretch, the short way round.
// Flights are worked out from the frame number, not added up frame by frame, so rounding doesn't build up.
class CameraPath {
public:
    // From its text: one waypoint a line, as a pose with blanks between its fields, and optionally one line
    // "speed M/S" for the speed. At least two waypoints, none where the last one is (nothing could fly between
    // them). Comments, line ends and logging as for the views.
    static core::Result<CameraPath> parse(std::string_view text, const char* source);

    size_t waypoint_count() const { return waypoints_.size(); }
    double length_m() const { return waypoints_.back().start_m; }
    double speed_mps() const { return speed_mps_; }
    // Over 0.
    void set_speed_mps(double speed) { speed_mps_ = speed; }

    // The pose this far along, the ends for anything past them.
    CameraPose at(double distance_m) const;
    // Frames a flight of step_s a frame takes: the first at the start, the last the first to reach the end.
    uint64_t flight_frames(double step_s) const;
    // The pose in a flight's frame, the first being 0.
    CameraPose at_flight_frame(uint64_t frame, double step_s) const;

private:
    struct Waypoint {
        DVec3 position{0.0};
        double yaw_rad = 0.0;
        double pitch_rad = 0.0;
        double start_m = 0.0; // how far along the path it is
    };

    CameraPath() = default;

    core::Array<Waypoint> waypoints_{core::heap_allocator(core::MemoryTag::Tools)};
    double speed_mps_ = kGallopMps;
};

} // namespace de::tools
