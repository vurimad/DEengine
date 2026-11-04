#include "app/frame_clock.h"
#include "app/framework.h"
#include "core/log.h"

#include <doctest/doctest.h>

#include <cmath>
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
    constexpr int kSettled = 120; // the frames it measures the rate over: the last of them goes in intervals
    const auto approx = [](double value) { return doctest::Approx(value).epsilon(1e-9); };
    const auto settle = [&](de::app::FrameClock& clock, double measured) {
        for (int i = 0; i < kSettled; ++i) {
            static_cast<void>(clock.step(measured, kHz));
        }
    };

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

    // A suspend is a long jump, within a couple of intervals of what it measured.
    CHECK(std::abs(rhythm.step(5.0, kHz) - 5.0) <= kInterval * 2.0);

    // A display a little off what it says, either way: steps of its real interval, no double or short
    // ones, and the time adds up.
    for (const double real : {1.005, 0.995}) {
        de::app::FrameClock off_display;
        settle(off_display, kInterval * real);
        CHECK(off_display.holds_rate());
        double off_handed_out = 0.0;
        for (int i = 0; i < 1000; ++i) {
            const double step = off_display.step(kInterval * real, kHz);
            CHECK(step == doctest::Approx(kInterval * real).epsilon(1e-6));
            off_handed_out += step;
        }
        CHECK(std::abs(off_handed_out - kInterval * real * 1000.0) <= kInterval);
        CHECK(off_display.step(kInterval * real * 10.0, kHz) >= kInterval * 9.0); // a real jump stays one
    }

    // Missed refreshes are no reason to stop: that's when it's needed. A frame of two intervals goes out
    // as two, and a hitch's catch-up frames don't matter either.
    de::app::FrameClock missing;
    settle(missing, kInterval);
    for (int window = 0; window < 4; ++window) {
        for (int i = 0; i < kSettled; ++i) {
            const double measured = i < 6 ? kInterval * 2.0 : kInterval;
            const double step = missing.step(measured, kHz);
            CHECK(step == approx(measured));
        }
        CHECK(missing.holds_rate());
    }
    for (int i = 0; i < kSettled; ++i) {
        const double measured = i == 0 ? kInterval * 5.0 : (i < 3 ? 0.001 : kInterval);
        static_cast<void>(missing.step(measured, kHz));
    }
    CHECK(missing.holds_rate());

    // A rhythm of three frames, or any that divides the window: on, and it stays on, even steps throughout.
    de::app::FrameClock three_beats;
    const auto beat = [&](int i) { return i % 3 == 2 ? kInterval * 3.0 - 0.002 : 0.001; };
    for (int i = 0; i < kSettled; ++i) {
        static_cast<void>(three_beats.step(beat(i), kHz));
    }
    CHECK(three_beats.holds_rate());
    for (int i = 0; i < kSettled * 6; ++i) {
        CHECK(three_beats.step(beat(i), kHz) == approx(kInterval));
    }

    // A lopsided rhythm, 1.6 and 0.4 intervals: what a frame doesn't use is carried, so the time adds up.
    de::app::FrameClock lopsided;
    for (int i = 0; i < kSettled; ++i) {
        static_cast<void>(lopsided.step(kInterval * (i % 2 == 0 ? 1.6 : 0.4), kHz));
    }
    CHECK(lopsided.holds_rate());
    double lopsided_handed_out = 0.0;
    for (int i = 0; i < 1000; ++i) {
        lopsided_handed_out += lopsided.step(kInterval * (i % 2 == 0 ? 1.6 : 0.4), kHz);
    }
    CHECK(std::abs(lopsided_handed_out - kInterval * 1000.0) <= kInterval * 3.0);

    // Long frames that aren't whole intervals: what a jump doesn't use is carried, not lost.
    de::app::FrameClock carrying;
    settle(carrying, kInterval);
    double carried_handed_out = 0.0;
    double carried_measured = 0.0;
    for (int i = 0; i < 1000; ++i) {
        const double measured = i % 20 == 0 ? kInterval * 2.45 : kInterval;
        carried_handed_out += carrying.step(measured, kHz);
        carried_measured += measured;
    }
    CHECK(carrying.holds_rate());
    CHECK(std::abs(carried_handed_out - carried_measured) <= kInterval);

    // Far ahead of the time measured, it slows down rather than run fast: steps never 0, since code may
    // divide by them.
    de::app::FrameClock ahead;
    settle(ahead, kInterval);
    double ahead_handed_out = 0.0;
    double ahead_measured = 0.0;
    for (int i = 0; i < 40; ++i) {
        const double step = ahead.step(kInterval * 0.25, kHz);
        CHECK(step > 0.0);
        ahead_handed_out += step;
        ahead_measured += kInterval * 0.25;
    }
    CHECK(std::abs(ahead_handed_out - ahead_measured) <= kInterval * 3.0 + 1e-9);

    // Without vsync, as measured, even at the rate.
    de::app::FrameClock no_vsync;
    for (int i = 0; i < kSettled * 2; ++i) {
        CHECK(no_vsync.step(0.003 + (i % 2) * 0.002, 0.0) == 0.003 + (i % 2) * 0.002);
    }

    // Not holding the rate (too much to draw): as measured.
    de::app::FrameClock slower;
    for (int i = 0; i < kSettled * 3; ++i) {
        const double measured = 0.0065 + (i % 2) * 0.001;
        CHECK(slower.step(measured, kHz) == measured);
    }

    // Near it but not on it (a variable-rate display at 235 fps): as measured, no double steps.
    de::app::FrameClock variable_rate;
    for (int i = 0; i < kSettled * 3; ++i) {
        CHECK(variable_rate.step(1.0 / 235.0, kHz) == 1.0 / 235.0);
    }

    // It stops, and starts again, as the rate does. A hitch alone doesn't stop it.
    de::app::FrameClock changing;
    settle(changing, kInterval);
    CHECK(changing.holds_rate());
    for (int i = 0; i < kSettled; ++i) {
        static_cast<void>(changing.step(i == 10 ? 0.2 : kInterval, kHz));
    }
    CHECK(changing.holds_rate());
    CHECK(changing.step(0.001, kHz) == approx(kInterval)); // an interval, not what it measured
    settle(changing, kInterval * 1.3);
    settle(changing, kInterval * 1.3);
    CHECK(!changing.holds_rate());
    settle(changing, kInterval); // back on the rate: back on
    settle(changing, kInterval);
    CHECK(changing.holds_rate());
    settle(changing, kInterval * 1.3);
    settle(changing, kInterval * 1.3);
    CHECK(changing.step(kInterval * 1.3, kHz) == kInterval * 1.3);

    // Every frame far longer than an interval: that's not on the intervals either.
    de::app::FrameClock heavy;
    settle(heavy, kInterval);
    CHECK(heavy.holds_rate());
    settle(heavy, kInterval * 4.3);
    settle(heavy, kInterval * 4.3);
    CHECK(!heavy.holds_rate());
    CHECK(heavy.step(kInterval * 4.3, kHz) == kInterval * 4.3);

    // Many long frames among even ones aren't hitches, even if the even ones look right.
    de::app::FrameClock often_long;
    settle(often_long, kInterval);
    for (int i = 0; i < kSettled * 2; ++i) {
        static_cast<void>(often_long.step(i % 2 == 0 ? kInterval : kInterval * 4.3, kHz));
    }
    CHECK(!often_long.holds_rate());

    // Another display: nothing measured on the last one counts, and no state is left without a rate.
    de::app::FrameClock moved;
    settle(moved, kInterval);
    CHECK(moved.holds_rate());
    const double at_60_hz = 1.0 / 60.0 + 0.0005;
    CHECK(moved.step(at_60_hz, 60.0) == at_60_hz);
    CHECK(!moved.holds_rate());
    de::app::FrameClock lost_rate;
    settle(lost_rate, kInterval);
    CHECK(lost_rate.step(0.001, 0.0) == 0.001);
    CHECK(!lost_rate.holds_rate());
}
