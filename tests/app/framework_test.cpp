#include "app/frame_clock.h"
#include "app/framework.h"
#include "core/log.h"

#include <doctest/doctest.h>

#include <initializer_list>
#include <string_view>

namespace {

// A command line that outlives the FrameworkArgs parsed from it (they point into it, as into main's).
struct CommandLine {
    char* argv[16] = {};
    int argc = 0;

    explicit CommandLine(std::initializer_list<const char*> flags) {
        REQUIRE(flags.size() < 16);
        argv[argc++] = const_cast<char*>("deengine");
        for (const char* flag : flags) {
            argv[argc++] = const_cast<char*>(flag);
        }
    }

    bool parse(de::app::FrameworkArgs& out) { return de::app::parse_args(argc, argv, out); }
};

} // namespace

TEST_CASE("the framework's flags are parsed") {
    CommandLine line({"--frames", "120", "--no-vsync", "--deadzone", "0.3", "--world", "00000000000000ab"});
    de::app::FrameworkArgs args;
    REQUIRE(line.parse(args));
    CHECK(args.frames == 120);
    CHECK_FALSE(args.vsync);
    CHECK(args.deadzone == doctest::Approx(0.3f));
    // The application's own flags are read through the framework's.
    REQUIRE(de::app::flag_value(args, "--world") != nullptr);
    CHECK(de::app::has_flag(args, "--no-vsync"));
    CHECK(de::app::flag_value(args, "--game-dir") == nullptr);
}

TEST_CASE("a deadzone that isn't a number in range is rejected") {
    const de::core::LogLevel saved = de::core::log_level();
    de::core::set_log_level(de::core::LogLevel::Fatal); // the rejections log errors by design
    for (const char* value : {"nan", "-0.1", "0.95", "abc", "0.2x", ""}) {
        CommandLine line({"--deadzone", value});
        de::app::FrameworkArgs args;
        CHECK_MESSAGE(!line.parse(args), value);
    }
    de::core::set_log_level(saved);
}

TEST_CASE("settings are read by name, past comments and blanks") {
    const std::string_view text = "\xEF\xBB\xBF"
                                  "first 1   # right after a byte order mark\r\n"
                                  "# engine settings\n"
                                  "\n"
                                  "world_size  7   # not this one\n"
                                  "  world   76d76b87e8562965   # the test world\r\n"
                                  "empty\n"
                                  "# world 0000000000000000\n";
    CHECK((de::app::setting(text, "world") == "76d76b87e8562965"));
    CHECK((de::app::setting(text, "world_size") == "7"));
    CHECK((de::app::setting(text, "first") == "1"));
    CHECK(de::app::setting(text, "empty").empty());
    CHECK(de::app::setting(text, "missing").empty());
    CHECK(de::app::setting(text, "engine").empty()); // only in a comment
}

TEST_CASE("the frame clock goes in whole refresh intervals while the rate holds, and hands out the time measured") {
    constexpr double kHz = 240.0;
    constexpr double kInterval = 1.0 / kHz;
    constexpr int kSettled = 64; // the frames it measures the rate over: the last of them goes in intervals
    const auto approx = [](double value) { return doctest::Approx(value).epsilon(1e-9); };

    // The loop's clock alternates, 1 and 7.33 ms, averaging the refresh rate: every step is an interval.
    de::app::FrameClock rhythm;
    for (int i = 0; i < kSettled - 1; ++i) {
        const double measured = i % 2 == 0 ? 0.001 : kInterval * 2.0 - 0.001;
        CHECK(rhythm.step(measured, kHz) == measured); // until it knows the rate holds: as measured
    }
    static_cast<void>(rhythm.step(kInterval * 2.0 - 0.001, kHz));
    double handed_out = 0.0;
    double measured_total = 0.0;
    for (int i = 0; i < 2400; ++i) {
        const double measured = i % 2 == 0 ? 0.001 : kInterval * 2.0 - 0.001;
        const double step = rhythm.step(measured, kHz);
        CHECK(step == approx(kInterval));
        handed_out += step;
        measured_total += measured;
    }
    CHECK(handed_out == doctest::Approx(measured_total).epsilon(1e-4)); // nothing runs slow or fast

    // A hitch is one jump of whole intervals, with what it measured, not spread over later frames.
    const double jump = rhythm.step(kInterval * 10.0, kHz);
    CHECK(jump >= kInterval * 9.0);
    CHECK(jump <= kInterval * 11.0);
    CHECK(rhythm.step(0.001, kHz) == approx(kInterval));

    // A suspend is a long jump, within an interval of what it measured.
    CHECK(rhythm.step(5.0, kHz) == doctest::Approx(5.0).epsilon(kInterval));

    // A display a little slower than it says: an extra interval now and then, so the time adds up.
    de::app::FrameClock slightly_off;
    for (int i = 0; i < kSettled; ++i) {
        static_cast<void>(slightly_off.step(kInterval * 1.03, kHz));
    }
    double off_handed_out = 0.0;
    for (int i = 0; i < 1000; ++i) {
        off_handed_out += slightly_off.step(kInterval * 1.03, kHz);
    }
    CHECK(off_handed_out == doctest::Approx(kInterval * 1030.0).epsilon(0.005));

    // Without vsync, as measured, even at the rate.
    de::app::FrameClock no_vsync;
    for (int i = 0; i < kSettled * 2; ++i) {
        CHECK(no_vsync.step(0.003 + (i % 2) * 0.002, 0.0) == 0.003 + (i % 2) * 0.002);
    }

    // Not holding the rate (too much to draw, or a variable-rate display): as measured.
    de::app::FrameClock slower;
    for (int i = 0; i < kSettled * 3; ++i) {
        const double measured = 0.0065 + (i % 2) * 0.001;
        CHECK(slower.step(measured, kHz) == measured);
    }

    // It stops, and starts again, as the rate does. Hitches alone don't stop it.
    de::app::FrameClock changing;
    for (int i = 0; i < kSettled; ++i) {
        static_cast<void>(changing.step(kInterval, kHz));
    }
    CHECK(changing.step(kInterval, kHz) == approx(kInterval));
    for (int i = 0; i < kSettled; ++i) {
        static_cast<void>(changing.step(i == 10 ? 0.2 : kInterval, kHz));
    }
    CHECK(changing.step(0.001, kHz) == approx(kInterval)); // an interval, not what it measured
    for (int i = 0; i < kSettled * 2; ++i) {
        static_cast<void>(changing.step(kInterval * 1.3, kHz));
    }
    CHECK(changing.step(kInterval * 1.3, kHz) == doctest::Approx(kInterval * 1.3));
}
