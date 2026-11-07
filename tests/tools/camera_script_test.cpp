#include "core/log.h"
#include "platform/input.h"
#include "tools/camera_script.h"
#include "tools/free_camera.h"
#include "world/render_extract.h"
#include "world/world.h"

#include <doctest/doctest.h>
#include <glm/trigonometric.hpp>

#include <cmath>
#include <cstdint>
#include <string_view>

using de::tools::CameraPath;
using de::tools::CameraPose;
using de::tools::find_camera_view;
using de::tools::parse_camera_pose;

namespace {

constexpr const char* kSource = "test.views";

// Errors are logged by design in the rejections.
struct QuietLog {
    de::core::LogLevel saved = de::core::log_level();
    QuietLog() { de::core::set_log_level(de::core::LogLevel::Fatal); }
    ~QuietLog() { de::core::set_log_level(saved); }
    QuietLog(const QuietLog&) = delete;
    QuietLog& operator=(const QuietLog&) = delete;
    QuietLog(QuietLog&&) = delete;
    QuietLog& operator=(QuietLog&&) = delete;
};

float rad(double deg) {
    return static_cast<float>(glm::radians(deg));
}

} // namespace

TEST_CASE("a pose is five numbers: meters, then degrees") {
    const auto pose = parse_camera_pose("1.5,-2,3e2,90,-30.5");
    REQUIRE(pose.ok());
    CHECK(pose->position.x == 1.5);
    CHECK(pose->position.y == -2.0);
    CHECK(pose->position.z == 300.0);
    CHECK(pose->yaw_rad == doctest::Approx(rad(90.0)));
    CHECK(pose->pitch_rad == doctest::Approx(rad(-30.5)));
}

TEST_CASE("a position 16 km away keeps every digit, which a float wouldn't") {
    constexpr double kFar = 16000.123456789;
    constexpr double kOtherWay = -15999.987654321;
    const auto pose = parse_camera_pose("16000.123456789,0.001,-15999.987654321,0,0");
    REQUIRE(pose.ok());
    CHECK(pose->position.x == kFar);
    CHECK(pose->position.z == kOtherWay);
    CHECK(static_cast<double>(static_cast<float>(kFar)) != kFar); // this test could tell

    const auto view = find_camera_view("far 16000.123456789 0.001 -15999.987654321 0 0\n", "far", kSource);
    REQUIRE(view.ok());
    CHECK(view->position.x == kFar);
    CHECK(view->position.z == kOtherWay);

    const auto path = CameraPath::parse("16000.123456789 0 0 0 0\n16001.123456789 0 0 0 0\n", kSource);
    REQUIRE(path.ok());
    CHECK(path->at(0.0).position.x == kFar);
    CHECK(path->at(1.0).position.x == 16001.123456789);
}

TEST_CASE("a pose that isn't five finite numbers in range is rejected") {
    // What's next to the rejections passes, so they fail for the reason they're there.
    for (const char* good : {"1,2,3,4,5", "-1,-2,-3,-400,-5", "0,0,0,0,88.8", "0,0,0,0,-88.8", "1e3,0,0,720,0"}) {
        CHECK_MESSAGE(parse_camera_pose(good).ok(), good);
    }
    const QuietLog quiet;
    for (const char* bad : {"",
                            "1",
                            "1,2,3,4",
                            "1,2,3,4,5,6",
                            ",2,3,4,5",
                            "1,,3,4,5",
                            "1,2,3,4,",
                            "a,2,3,4,5",
                            "1,2,3,4,5x",
                            " 1,2,3,4,5",
                            "1,2,3,4,5 ",
                            "1, 2,3,4,5",
                            "nan,2,3,4,5",
                            "1,inf,3,4,5",
                            "1,2,-inf,4,5",
                            "1,2,3,nan,5",
                            "1,2,3,4,nan",
                            "1e999,2,3,4,5",
                            "1,2,3,4,88.9",
                            "1,2,3,4,-88.9",
                            "1,2,3,4,90",
                            "1 2 3 4 5"}) {
        CHECK_MESSAGE(!parse_camera_pose(bad).ok(), bad);
    }
}

TEST_CASE("what the free camera holds is what a pose may ask for") {
    CHECK(glm::radians(88.8f) <= de::tools::FreeCamera::kMaxPitchRad);
    CHECK(glm::radians(88.9f) > de::tools::FreeCamera::kMaxPitchRad);
}

TEST_CASE("a view is found by name, past comments, blanks and line ends") {
    const std::string_view text = "\xEF\xBB\xBF"
                                  "first 1 2 3 4 5   # right after a byte order mark\r\n"
                                  "# second 9 9 9 9 9\n"
                                  "\n"
                                  "   second\t10 20 30 40 50\r\n"
                                  "last 7 8 9 0 0";
    const auto first = find_camera_view(text, "first", kSource);
    REQUIRE(first.ok());
    CHECK(first->position.x == 1.0);
    const auto second = find_camera_view(text, "second", kSource);
    REQUIRE(second.ok());
    CHECK(second->position.y == 20.0);
    CHECK(second->yaw_rad == doctest::Approx(rad(40.0)));
    CHECK(second->pitch_rad == doctest::Approx(rad(50.0)));
    const auto last = find_camera_view(text, "last", kSource); // no end of line
    REQUIRE(last.ok());
    CHECK(last->position.z == 9.0);
}

TEST_CASE("a views file is checked whole, whichever view is asked for") {
    constexpr std::string_view kGood = "a 1 2 3 4 5\nb 1 2 3 4 5\n";
    REQUIRE(find_camera_view(kGood, "a", kSource).ok());
    const QuietLog quiet;
    CHECK_FALSE(find_camera_view(kGood, "c", kSource).ok()); // no such view
    CHECK_FALSE(find_camera_view("", "a", kSource).ok());
    CHECK_FALSE(find_camera_view("# nothing but this\n", "a", kSource).ok());
    for (const std::string_view line : {"b 1 2 3 4\n", "b 1 2 3 4 5 6\n", "b\n", "b x 2 3 4 5\n", "b 1 2 3 4 nan\n",
                                        "b 1 2 3 4 89\n", "1 2 3 4 5\n", "a 9 9 9 9 9\n"}) {
        // The view asked for comes first, and is good; a bad line after it (a name twice, the last) still fails.
        const char* const good_first = "a 1 2 3 4 5\n";
        char text[64] = {};
        std::string_view(good_first).copy(text, 12);
        line.copy(text + 12, line.size());
        CHECK_MESSAGE(!find_camera_view(std::string_view(text, 12 + line.size()), "a", kSource).ok(), line.data());
    }
}

TEST_CASE("a path flies straight at its speed, turning the short way round") {
    const auto path = CameraPath::parse("# a ride\n"
                                        "0 0 0 170 10\n"
                                        "100 0 0 -170 -10   # 20 degrees of turn, across the seam\n"
                                        "100 0 50 -170 -10\n",
                                        kSource);
    REQUIRE(path.ok());
    CHECK(path->waypoint_count() == 3);
    CHECK(path->length_m() == 150.0);
    CHECK(path->speed_mps() == de::tools::kGallopMps);

    const CameraPose start = path->at(0.0);
    CHECK(start.position.x == 0.0);
    CHECK(start.yaw_rad == doctest::Approx(rad(170.0)));
    CHECK(start.pitch_rad == doctest::Approx(rad(10.0)));

    const CameraPose middle = path->at(50.0);
    CHECK(middle.position.x == 50.0);
    CHECK(middle.yaw_rad == doctest::Approx(rad(180.0))); // not back through 0
    CHECK(middle.pitch_rad == doctest::Approx(0.0f).epsilon(1e-6));

    const CameraPose corner = path->at(100.0);
    CHECK(corner.position.x == 100.0);
    CHECK(corner.position.z == 0.0);

    const CameraPose second_stretch = path->at(125.0);
    CHECK(second_stretch.position.x == 100.0);
    CHECK(second_stretch.position.z == 25.0);

    const CameraPose end = path->at(150.0);
    CHECK(end.position.z == 50.0);
    // Past the ends: the ends.
    CHECK(path->at(-5.0).position.x == 0.0);
    CHECK(path->at(1e9).position.z == 50.0);
}

TEST_CASE("a flight's frames start at the start and end at the first to reach the end") {
    auto path = CameraPath::parse("speed 10\n0 0 0 0 0\n100 0 0 0 0\n", kSource);
    REQUIRE(path.ok());
    CHECK(path->speed_mps() == 10.0);
    // A meter a frame: 0 m, 1 m ... 100 m.
    CHECK(path->flight_frames(0.1) == 101);
    CHECK(path->at_flight_frame(0, 0.1).position.x == 0.0);
    CHECK(path->at_flight_frame(37, 0.1).position.x == doctest::Approx(37.0));
    CHECK(path->at_flight_frame(100, 0.1).position.x == 100.0);
    // Between whole steps: the last frame is the first past it, held at the end.
    path->set_speed_mps(30.0); // 3 m a frame: 34 steps reach 102
    CHECK(path->flight_frames(0.1) == 35);
    CHECK(path->at_flight_frame(33, 0.1).position.x == doctest::Approx(99.0));
    CHECK(path->at_flight_frame(34, 0.1).position.x == 100.0);
}

TEST_CASE("a flight's rounding doesn't build up over a long ride") {
    const auto path = CameraPath::parse("speed 13\n0 0 0 0 0\n1000000 0 0 0 0\n", kSource);
    REQUIRE(path.ok());
    constexpr double kStep = 1.0 / 60.0;
    constexpr uint64_t kFrame = 3'000'000;
    // Added up frame by frame, this is about 1e-5 m off; worked out from the frame, one rounding.
    CHECK(path->at_flight_frame(kFrame, kStep).position.x == doctest::Approx(650000.0).epsilon(1e-12));
}

TEST_CASE("a path needs two waypoints apart, and at most one good speed") {
    REQUIRE(CameraPath::parse("speed 5\n0 0 0 0 0\n1 0 0 0 0\n", kSource).ok()); // what the rejections differ from
    const QuietLog quiet;
    for (const char* bad :
         {"", "# no waypoints\n", "speed 5\n", "0 0 0 0 0\n", "speed 5\n0 0 0 0 0\n",
          "0 0 0 0 0\n0 0 0 0 0\n",             // nothing to fly between
          "0 0 0 0 0\n1 0 0 0 0\n1 0 0 90 0\n", // a turn on the spot
          "speed 5\nspeed 6\n0 0 0 0 0\n1 0 0 0 0\n", "speed\n0 0 0 0 0\n1 0 0 0 0\n",
          "speed 0\n0 0 0 0 0\n1 0 0 0 0\n", "speed -1\n0 0 0 0 0\n1 0 0 0 0\n", "speed nan\n0 0 0 0 0\n1 0 0 0 0\n",
          "speed 5 6\n0 0 0 0 0\n1 0 0 0 0\n", "0 0 0 0\n1 0 0 0 0\n", "0 0 0 0 0 0\n1 0 0 0 0\n",
          "x 0 0 0 0\n1 0 0 0 0\n", "0 0 0 0 0\n1 0 0 0 89\n"}) {
        CHECK_MESSAGE(!CameraPath::parse(bad, kSource).ok(), bad);
    }
}

TEST_CASE("a pose set on the free camera is where it is") {
    de::tools::FreeCamera camera({0.0, 0.0, 0.0}, 0.0f, 0.0f);
    camera.set_pose({.position = {16000.123456789, 2.0, -3.0}, .yaw_rad = 0.5f, .pitch_rad = -0.25f});
    CHECK(camera.position().x == 16000.123456789);
    const de::Vec3 forward = camera.rotation() * de::Vec3(0.0f, 0.0f, -1.0f);
    CHECK(forward.y == doctest::Approx(std::sin(-0.25f)));
    CHECK(forward.x == doctest::Approx(-std::sin(0.5f) * std::cos(0.25f)));
}

TEST_CASE("a pose 16 km away reaches the frame the renderer gets, to a hair") {
    const auto pose = parse_camera_pose("16000.123456789,2.5,-15999.987654321,0,0");
    REQUIRE(pose.ok());
    auto world = de::world::World::create({});
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    const flecs::entity eye = ecs.entity("eye").set(de::world::Camera{.vertical_fov_rad = 1.0f});
    ecs.set(de::world::ActiveCamera{.entity = eye.id()});
    de::platform::Input input;
    const de::tools::FreeCameraActions actions = de::tools::add_free_camera_actions(input);
    input.sample();
    de::tools::FreeCamera camera({0.0, 0.0, 0.0}, 0.0f, 0.0f);
    camera.set_pose(*pose);
    camera.update(input, actions, 1.0f / 60.0f, eye);
    CHECK(eye.get<de::world::WorldTransform>().position.x == 16000.123456789);
    CHECK(eye.get<de::world::WorldTransform>().position.z == -15999.987654321);

    static_cast<void>(world->advance(1.0 / 60.0));
    const de::world::RenderFrame& frame = world->render_frame();
    REQUIRE(frame.camera.valid);
    // Floats, but relative to the render origin: within 0.05 mm, where floats from the world's origin are up to a
    // millimeter off this far out.
    constexpr double kWithin = 5e-5;
    CHECK(std::abs(frame.origin.x + static_cast<double>(frame.camera.transform[3]) - 16000.123456789) < kWithin);
    CHECK(std::abs(frame.origin.z + static_cast<double>(frame.camera.transform[11]) + 15999.987654321) < kWithin);
    CHECK(std::abs(static_cast<double>(static_cast<float>(16000.123456789)) - 16000.123456789) >
          kWithin); // it could tell
}
